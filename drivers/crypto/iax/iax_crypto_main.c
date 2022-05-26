// SPDX-License-Identifier: GPL-2.0
/* Copyright(c) 2021 Intel Corporation. All rights rsvd. */

#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/pci.h>
#include <linux/device.h>
#include <linux/iommu.h>
#include <linux/auxiliary_bus.h>
#include <linux/uacce.h>
#include <uapi/linux/idxd.h>
#include <linux/highmem.h>
#include <crypto/internal/acompress.h>

#include "registers.h"
#include "idxd.h"
#include "iax_crypto.h"

#define IAX_CRYPTO_VER			"1.0"

#define IAX_CRYPTO_WQ_NAME		"iax_crypto"
#define IAX_ALG_PRIORITY		300
#define IAX_AECS_ALIGN			32

/* IAX completion timeout value in tsc units */
static unsigned int iax_completion_timeout = IAX_COMPLETION_TIMEOUT;

module_param_named(iax_completion_timeout, iax_completion_timeout, uint, 0644);
MODULE_PARM_DESC(iax_completion_timeout, "IAX completion timeout (1000000 cycles default)");

/* Verify results of IAX compress or not */
static bool iax_verify_compress = 1;

module_param_named(iax_verify_compress, iax_verify_compress, bool, 0644);
MODULE_PARM_DESC(iax_verify_compress,
		 "Verify IAX compression (value = 1) or not (value = 0)");

static LIST_HEAD(iax_devices);
static DEFINE_SPINLOCK(iax_devices_lock);

static struct crypto_comp *deflate_generic_tfm;

static int iax_wqs_get(struct iax_device *iax_device)
{
	struct iax_wq *iax_wq;
	int n_wqs = 0;
	int ret = 0;

	list_for_each_entry(iax_wq, &iax_device->wqs, list) {
		mutex_lock(&iax_wq->wq->wq_lock);
		ret = idxd_wq_alloc_resources(iax_wq->wq);
		if (ret < 0) {
			pr_err("%s: WQ resource alloc failed for iax device %d, wq %d: ret=%d\n", __func__, iax_device->idxd->id, iax_wq->wq->id, ret);
			mutex_unlock(&iax_wq->wq->wq_lock);
			return ret;
		}
		idxd_wq_get(iax_wq->wq);
		mutex_unlock(&iax_wq->wq->wq_lock);
		n_wqs++;
	}

	return n_wqs;
}

static void iax_wqs_put(struct iax_device *iax_device)
{
	struct iax_wq *iax_wq;

	list_for_each_entry(iax_wq, &iax_device->wqs, list) {
		mutex_lock(&iax_wq->wq->wq_lock);
		idxd_wq_free_resources(iax_wq->wq);
		idxd_wq_put(iax_wq->wq);
		mutex_unlock(&iax_wq->wq->wq_lock);
	}
}

static int iax_all_wqs_get(void)
{
	struct iax_device *iax_device;
	int n_wqs = 0;
	int ret;

	spin_lock(&iax_devices_lock);
	list_for_each_entry(iax_device, &iax_devices, list) {
		ret = iax_wqs_get(iax_device);
		if (ret < 0) {
			spin_unlock(&iax_devices_lock);
			return ret;
		}
		n_wqs += ret;
	}
	spin_unlock(&iax_devices_lock);

	return n_wqs;
}

static void iax_all_wqs_put(void)
{
	struct iax_device *iax_device;

	spin_lock(&iax_devices_lock);
	list_for_each_entry(iax_device, &iax_devices, list)
		iax_wqs_put(iax_device);
	spin_unlock(&iax_devices_lock);
}

static bool iax_crypto_enabled = false;
static int iax_crypto_enable(const char *val, const struct kernel_param *kp)
{
	int ret = 0;

        if (val[0] == '0') {
		iax_crypto_enabled = false;
		iax_all_wqs_put();
	} else if (val[0] == '1') {
		ret = iax_all_wqs_get();
		if (ret == 0) {
			pr_info("%s: no wqs available, not enabling iax_crypto\n", __func__);
			return ret;
		} else if (ret < 0) {
			pr_err("%s: iax_crypto enable failed: ret=%d\n", __func__, ret);
			return ret;
		} else
			iax_crypto_enabled = true;
	} else {
		pr_err("%s: iax_crypto failed, bad enable val: ret=%d\n", __func__, -EINVAL);
		return -EINVAL;
	}

	pr_info("%s: iax_crypto now %s\n", __func__,
		iax_crypto_enabled ? "ENABLED" : "DISABLED");

	return ret;
}
static const struct kernel_param_ops enable_ops = {
	.set = iax_crypto_enable,
	.get = param_get_bool,
};
module_param_cb(iax_crypto_enable, &enable_ops, &iax_crypto_enabled, 0644);
MODULE_PARM_DESC(iax_crypto_enable, "Enable (value = 1) or disable (value = 0) iax_crypto");

int wq_stats_show(struct seq_file *m, void *v)
{
	struct iax_device *iax_device;

	spin_lock(&iax_devices_lock);

	global_stats_show(m);

	list_for_each_entry(iax_device, &iax_devices, list)
		device_stats_show(m, iax_device);

	spin_unlock(&iax_devices_lock);

	return 0;
}

int iax_crypto_stats_reset(void *data, u64 value)
{
	struct iax_device *iax_device;

	reset_iax_crypto_stats();

	spin_lock(&iax_devices_lock);

	list_for_each_entry(iax_device, &iax_devices, list)
		reset_device_stats(iax_device);

	spin_unlock(&iax_devices_lock);

	return 0;
}

static struct iax_device *iax_device_alloc(void)
{
	struct iax_device *iax_device;

	iax_device = kzalloc(sizeof(*iax_device), GFP_KERNEL);
	if (!iax_device)
		return NULL;

	INIT_LIST_HEAD(&iax_device->wqs);

	return iax_device;
}

static void iax_device_free(struct iax_device *iax_device)
{
	struct iax_wq *iax_wq, *next;

	list_for_each_entry_safe(iax_wq, next, &iax_device->wqs, list) {
		list_del(&iax_wq->list);
		kfree(iax_wq); // zzzz do this in original code too
	}

	kfree(iax_device);
}

static void free_iax_devices(void)
{
	struct iax_device *iax_device, *next;

	spin_lock(&iax_devices_lock);
	list_for_each_entry_safe(iax_device, next, &iax_devices, list) {
		list_del(&iax_device->list);
		iax_device_free(iax_device);
	}
	spin_unlock(&iax_devices_lock);
}

/* IAX number of iax instances found */
static unsigned int nr_iax;
static unsigned int nr_cpus;
static unsigned int nr_nodes;

/* Number of physical cpus sharing each iax instance */
static unsigned int cpus_per_iax;

/* Per-cpu lookup table for balanced wqs */
static struct idxd_wq * __percpu *wq_table;

/*
 * Given a cpu, find the closest IAX instance.  The idea is to try to
 * choose the most appropriate IAX instance for a caller and spread
 * available workqueues around to clients.
 */
static inline int cpu_to_iax(int cpu)
{
	const struct cpumask *node_cpus;
	int node, n_cpus = 0, test_cpu, iax;
	int nr_iax_per_node;

	nr_iax_per_node = nr_iax / nr_nodes;

	for_each_online_node(node) {
		node_cpus = cpumask_of_node(node);
		if (!cpumask_test_cpu(cpu, node_cpus))
			continue;

		iax = node * nr_iax_per_node;

		for_each_cpu(test_cpu, node_cpus) {
			if (test_cpu == cpu)
				return iax;

			n_cpus++;
			if ((n_cpus % cpus_per_iax) == 0)
				iax++;
		}
	}

	return -1;
}

static bool iax_has_wq(struct iax_device *iax_device, struct idxd_wq *wq)
{
	struct iax_wq *iax_wq;

	list_for_each_entry(iax_wq, &iax_device->wqs, list) {
		if (iax_wq->wq == wq)
			return true;
	}

	return false;
}

const u32 fixed_ll_sym[286] = {
	0x40030, 0x40031, 0x40032, 0x40033, 0x40034, 0x40035, 0x40036, 0x40037,
	0x40038, 0x40039, 0x4003A, 0x4003B, 0x4003C, 0x4003D, 0x4003E, 0x4003F,
	0x40040, 0x40041, 0x40042, 0x40043, 0x40044, 0x40045, 0x40046, 0x40047,
	0x40048, 0x40049, 0x4004A, 0x4004B, 0x4004C, 0x4004D, 0x4004E, 0x4004F,
	0x40050, 0x40051, 0x40052, 0x40053, 0x40054, 0x40055, 0x40056, 0x40057,
	0x40058, 0x40059, 0x4005A, 0x4005B, 0x4005C, 0x4005D, 0x4005E, 0x4005F,
	0x40060, 0x40061, 0x40062, 0x40063, 0x40064, 0x40065, 0x40066, 0x40067,
	0x40068, 0x40069, 0x4006A, 0x4006B, 0x4006C, 0x4006D, 0x4006E, 0x4006F,
	0x40070, 0x40071, 0x40072, 0x40073, 0x40074, 0x40075, 0x40076, 0x40077,
	0x40078, 0x40079, 0x4007A, 0x4007B, 0x4007C, 0x4007D, 0x4007E, 0x4007F,
	0x40080, 0x40081, 0x40082, 0x40083, 0x40084, 0x40085, 0x40086, 0x40087,
	0x40088, 0x40089, 0x4008A, 0x4008B, 0x4008C, 0x4008D, 0x4008E, 0x4008F,
	0x40090, 0x40091, 0x40092, 0x40093, 0x40094, 0x40095, 0x40096, 0x40097,
	0x40098, 0x40099, 0x4009A, 0x4009B, 0x4009C, 0x4009D, 0x4009E, 0x4009F,
	0x400A0, 0x400A1, 0x400A2, 0x400A3, 0x400A4, 0x400A5, 0x400A6, 0x400A7,
	0x400A8, 0x400A9, 0x400AA, 0x400AB, 0x400AC, 0x400AD, 0x400AE, 0x400AF,
	0x400B0, 0x400B1, 0x400B2, 0x400B3, 0x400B4, 0x400B5, 0x400B6, 0x400B7,
	0x400B8, 0x400B9, 0x400BA, 0x400BB, 0x400BC, 0x400BD, 0x400BE, 0x400BF,
	0x48190, 0x48191, 0x48192, 0x48193, 0x48194, 0x48195, 0x48196, 0x48197,
	0x48198, 0x48199, 0x4819A, 0x4819B, 0x4819C, 0x4819D, 0x4819E, 0x4819F,
	0x481A0, 0x481A1, 0x481A2, 0x481A3, 0x481A4, 0x481A5, 0x481A6, 0x481A7,
	0x481A8, 0x481A9, 0x481AA, 0x481AB, 0x481AC, 0x481AD, 0x481AE, 0x481AF,
	0x481B0, 0x481B1, 0x481B2, 0x481B3, 0x481B4, 0x481B5, 0x481B6, 0x481B7,
	0x481B8, 0x481B9, 0x481BA, 0x481BB, 0x481BC, 0x481BD, 0x481BE, 0x481BF,
	0x481C0, 0x481C1, 0x481C2, 0x481C3, 0x481C4, 0x481C5, 0x481C6, 0x481C7,
	0x481C8, 0x481C9, 0x481CA, 0x481CB, 0x481CC, 0x481CD, 0x481CE, 0x481CF,
	0x481D0, 0x481D1, 0x481D2, 0x481D3, 0x481D4, 0x481D5, 0x481D6, 0x481D7,
	0x481D8, 0x481D9, 0x481DA, 0x481DB, 0x481DC, 0x481DD, 0x481DE, 0x481DF,
	0x481E0, 0x481E1, 0x481E2, 0x481E3, 0x481E4, 0x481E5, 0x481E6, 0x481E7,
	0x481E8, 0x481E9, 0x481EA, 0x481EB, 0x481EC, 0x481ED, 0x481EE, 0x481EF,
	0x481F0, 0x481F1, 0x481F2, 0x481F3, 0x481F4, 0x481F5, 0x481F6, 0x481F7,
	0x481F8, 0x481F9, 0x481FA, 0x481FB, 0x481FC, 0x481FD, 0x481FE, 0x481FF,
	0x38000, 0x38001, 0x38002, 0x38003, 0x38004, 0x38005, 0x38006, 0x38007,
	0x38008, 0x38009, 0x3800A, 0x3800B, 0x3800C, 0x3800D, 0x3800E, 0x3800F,
	0x38010, 0x38011, 0x38012, 0x38013, 0x38014, 0x38015, 0x38016, 0x38017,
	0x400C0, 0x400C1, 0x400C2, 0x400C3, 0x400C4, 0x400C5
};

const u32 fixed_d_sym[30] = {
	0x28000, 0x28001, 0x28002, 0x28003, 0x28004, 0x28005, 0x28006, 0x28007,
	0x28008, 0x28009, 0x2800A, 0x2800B, 0x2800C, 0x2800D, 0x2800E, 0x2800F,
	0x28010, 0x28011, 0x28012, 0x28013, 0x28014, 0x28015, 0x28016, 0x28017,
	0x28018, 0x28019, 0x2801A, 0x2801B, 0x2801C, 0x2801D
};

static int iax_aecs_alloc(struct iax_device *iax_device)
{
	size_t size = sizeof(struct aecs_table_record) + IAX_AECS_ALIGN;
	struct device *dev = &iax_device->idxd->pdev->dev;
	u32 bfinal = 1;
	u32 offset;

	iax_device->aecs_table_unaligned = dma_alloc_coherent(dev, size,
							      &iax_device->aecs_table_addr_unaligned, GFP_KERNEL);
	if (!iax_device->aecs_table_unaligned) {
		iax_device_free(iax_device);
		return -ENOMEM;
	}
	iax_device->aecs_table = PTR_ALIGN(iax_device->aecs_table_unaligned, IAX_AECS_ALIGN);
	iax_device->aecs_table_addr = ALIGN(iax_device->aecs_table_addr_unaligned, IAX_AECS_ALIGN);

	/* Configure aecs table using fixed Huffman table */
	iax_device->aecs_table->crc = 0;
	iax_device->aecs_table->xor_checksum = 0;
	offset = iax_device->aecs_table->num_output_accum_bits / 8;
	iax_device->aecs_table->output_accum[offset] = DYNAMIC_HDR | bfinal;
	iax_device->aecs_table->num_output_accum_bits = DYNAMIC_HDR_SIZE;

	/* Add Huffman table to aecs */
	memcpy(iax_device->aecs_table->ll_sym, fixed_ll_sym, sizeof(fixed_ll_sym));
	memcpy(iax_device->aecs_table->d_sym, fixed_d_sym, sizeof(fixed_d_sym));

	return 0;
}

static void iax_aecs_free(struct iax_device *iax_device)
{
	size_t size = sizeof(struct aecs_table_record) + IAX_AECS_ALIGN;
	struct device *dev = &iax_device->idxd->pdev->dev;

	dma_free_coherent(dev, size,
			  iax_device->aecs_table_unaligned, iax_device->aecs_table_addr_unaligned);
}

static struct iax_device *add_iax_device(struct idxd_device *idxd)
{
	struct iax_device *iax_device;

	iax_device = iax_device_alloc();
	if (!iax_device)
		return NULL;

	iax_device->idxd = idxd;

	if (iax_aecs_alloc(iax_device) < 0)
		return NULL;

	list_add_tail(&iax_device->list, &iax_devices);

	nr_iax++;

	return iax_device;
}

static void del_iax_device(struct iax_device *iax_device)
{
	iax_aecs_free(iax_device);

	list_del(&iax_device->list);

	iax_device_free(iax_device);

	nr_iax--;
}

static int add_iax_wq(struct iax_device *iax_device, struct idxd_wq *wq)
{
	struct iax_wq *iax_wq;

	iax_wq = kzalloc(sizeof(*iax_wq), GFP_KERNEL);
	if (!iax_wq)
		return -ENOMEM;

	iax_wq->wq = wq;
	iax_wq->iax_device = iax_device;
	wq->private_data = iax_wq;

	list_add_tail(&iax_wq->list, &iax_device->wqs);

	iax_device->n_wq++;

	pr_debug("%s: added wq %p to iax %p, n_wq %d\n", __func__, wq, iax_device, iax_device->n_wq);

	return 0;
}

static void del_iax_wq(struct iax_device *iax_device, struct idxd_wq *wq)
{
	struct iax_wq *iax_wq;

	list_for_each_entry(iax_wq, &iax_device->wqs, list) {
		if (iax_wq->wq == wq) {
			list_del(&iax_wq->list);
			iax_device->n_wq--;

			pr_debug("%s: removed wq %p from iax_device %p, n_wq %d, nr_iax %d\n", __func__, wq, iax_device, iax_device->n_wq, nr_iax);

			if (iax_device->n_wq == 0)
				del_iax_device(iax_device);
			break;
		}
	}
}

static int save_iax_wq(struct idxd_wq *wq)
{
	struct iax_device *iax_device, *found = NULL;
	int ret = 0;

	spin_lock(&iax_devices_lock);
	list_for_each_entry(iax_device, &iax_devices, list) {
		if (iax_device->idxd == wq->idxd) {
			/*
			 * Check to see that we don't already have this wq.
			 * Shouldn't happen but we don't control probing.
			 */
			if (iax_has_wq(iax_device, wq)) {
				pr_warn("%s: same wq probed multiple times for iax_device %p\n", __func__, iax_device);
				goto out;
			}

			found = iax_device;

			ret = add_iax_wq(iax_device, wq);
			if (ret)
				goto out;

			break;
		}
	}

	if (!found) {
		struct iax_device *new;

		new = add_iax_device(wq->idxd);
		if (!new) {
			ret = -ENOMEM;
			goto out;
		}

		ret = add_iax_wq(new, wq);
		if (ret) {
			del_iax_device(new);
			goto out;
		}
	}

	BUG_ON(nr_iax == 0);

	cpus_per_iax = nr_cpus / nr_iax;
out:
	spin_unlock(&iax_devices_lock);

	return 0;
}

static void clear_wq_table(void)
{
	int cpu;

	for (cpu = 0; cpu < nr_cpus; cpu++)
		*per_cpu_ptr(wq_table, cpu) = NULL;

	pr_debug("%s: cleared wq table\n", __func__);
}

static void remove_iax_wq(struct idxd_wq *wq)
{
	struct iax_device *iax_device;

	spin_lock(&iax_devices_lock);
	list_for_each_entry(iax_device, &iax_devices, list) {
		if (iax_has_wq(iax_device, wq)) {
			del_iax_wq(iax_device, wq);
			if (nr_iax == 0)
				clear_wq_table();
			break;
		}
	}
	spin_unlock(&iax_devices_lock);

	if (nr_iax)
		cpus_per_iax = nr_cpus / nr_iax;
	else
		cpus_per_iax = 0;
}

static struct idxd_wq *request_iax_wq(int iax)
{
	struct iax_device *iax_device, *found_device = NULL;
	struct idxd_wq *bkup_wq = NULL, *found_wq = NULL;
	int cur_iax = 0, cur_wq = 0, cur_bkup;
	struct iax_wq *iax_wq;

	spin_lock(&iax_devices_lock);
	list_for_each_entry(iax_device, &iax_devices, list) {
		if (cur_iax != iax) {
			cur_iax++;
			continue;
		}

		found_device = iax_device;
		pr_debug("%s: getting wq from iax_device %p (%d)\n", __func__, found_device, cur_iax);
		break;
	}

	if (!found_device) {
		found_device = list_first_entry_or_null(&iax_devices,
							struct iax_device, list);
		if (!found_device) {
			pr_warn("%s: couldn't find any iax devices with wqs!\n", __func__);
			goto out;
		}
		cur_iax = 0;
		pr_debug("%s: getting wq from only iax_device %p (%d)\n", __func__, found_device, cur_iax);
	}

	list_for_each_entry(iax_wq, &found_device->wqs, list) {
		/* Prefer unused wq but use if we can't find one */
		if (idxd_wq_refcount(iax_wq->wq) > 0) {
			bkup_wq = iax_wq->wq;
			cur_bkup = cur_wq;
		} else {
			pr_debug("%s: returning unused wq %p (%d) from iax device %p (%d)\n", __func__, iax_wq->wq, cur_wq, found_device, cur_iax);
			found_wq = iax_wq->wq;
			goto out;
		}
		cur_wq++;
	}

	if (bkup_wq) {
		pr_debug("%s: returning used wq %p (%d) from iax device %p (%d)\n", __func__, bkup_wq, cur_bkup, found_device, cur_iax);
		found_wq = bkup_wq;
		goto out;
	}
out:
	spin_unlock(&iax_devices_lock);

	return found_wq;
}

static inline int check_completion(struct iax_completion_record *comp,
				   bool compress)
{
	char *op_str = compress ? "compress" : "decompress";
	int ret = 0;

	while (!comp->status)
		cpu_relax();

	if (comp->status != IAX_COMP_SUCCESS) {
		if (comp->status == IAX_ERROR_WATCHDOG_EXPIRED) {
			ret = -ETIMEDOUT;
			pr_warn("%s: %s timed out, size=0x%x\n",
				__func__, op_str, comp->output_size);
			update_completion_timeout_errs();
			goto out;
		}

		if (comp->status == IAX_ANALYTICS_ERROR &&
		    comp->error_code == IAX_ERROR_COMP_BUF_OVERFLOW &&
		    compress == true) {
			ret = -E2BIG;
			pr_debug("%s: compressed size > uncompressed size, not compressing, size=0x%x\n", __func__, comp->output_size);
			update_completion_comp_buf_overflow_errs();
			goto out;
		}

		ret = -EINVAL;
		pr_err("%s: iax %s status=0x%x, error=0x%x, size=0x%x\n",
		       __func__, op_str, ret, comp->error_code, comp->output_size);
		print_hex_dump(KERN_INFO, "cmp-rec: ", DUMP_PREFIX_OFFSET, 8, 1, comp, 64, 0);
		update_completion_einval_errs();

		goto out;
	}
out:
	return ret;
}

static int iax_compress(struct crypto_tfm *tfm,
			const u8 *src, unsigned int slen,
			u8 *dst, unsigned int *dlen)
{
	dma_addr_t src_addr, dst_addr;
	struct idxd_desc *idxd_desc;
	struct iax_hw_desc *desc;
	struct iax_wq *iax_wq;
	u32 compression_crc;
	struct idxd_wq *wq;
	struct device *dev;
	int ret = 0;

	wq = *per_cpu_ptr(wq_table, smp_processor_id());
	if (!wq) {
		pr_err("%s: no wq configured for cpu=%d\n", __func__, smp_processor_id());
		return -ENODEV;
	}
	dev = &wq->idxd->pdev->dev;

	iax_wq = wq->private_data;

	pr_debug("%s: using wq for cpu=%d = wq %p\n", __func__, smp_processor_id(), wq);

	idxd_desc = idxd_alloc_desc(wq, IDXD_OP_BLOCK);
	if (IS_ERR(idxd_desc)) {
		pr_err("%s: idxd descriptor allocation failed\n", __func__);
		pr_warn("%s: iax compress failed: ret=%ld\n", __func__, PTR_ERR(desc));

		return PTR_ERR(idxd_desc);
	}
	desc = idxd_desc->iax_hw;

	desc->flags = IDXD_OP_FLAG_CRAV | IDXD_OP_FLAG_RCR |
		IDXD_OP_FLAG_RD_SRC2_AECS | IDXD_OP_FLAG_CC;
	desc->opcode = IAX_OPCODE_COMPRESS;
	desc->compr_flags = IAX_COMP_FLAGS;
#ifdef SPR_E0
	desc->priv = 1;
#else
	desc->priv = 0;
#endif

	src_addr = dma_map_single(dev, (void *)src, slen, DMA_TO_DEVICE);
	pr_debug("%s: dma_map_single, src_addr %llx, dev %p, src %p, slen %d\n", __func__, src_addr, dev, src, slen);
	if (unlikely(dma_mapping_error(dev, src_addr))) {
		pr_debug("%s: dma_map_single err, exiting\n", __func__);
		ret = -ENOMEM;
		goto err_map_src;
	}

	dst_addr = dma_map_single(dev, (void *)dst, *dlen, DMA_FROM_DEVICE);
	pr_debug("%s: dma_map_single, dst_addr %llx, dev %p, dst %p, *dlen %d\n", __func__, dst_addr, dev, dst, *dlen);
	if (unlikely(dma_mapping_error(dev, dst_addr))) {
		pr_debug("%s: dma_map_single err, exiting\n", __func__);
		ret = -ENOMEM;
		goto err_map_dst;
	}

	desc->src1_addr = (u64)src_addr;
	desc->src1_size = slen;
	desc->dst_addr = (u64)dst_addr;
	desc->max_dst_size = *dlen;
	desc->src2_addr = iax_wq->iax_device->aecs_table_addr;
	desc->src2_size = sizeof(struct aecs_table_record);
	desc->completion_addr = idxd_desc->compl_dma;

	ret = idxd_submit_desc(wq, idxd_desc);
	if (ret) {
		pr_warn("%s: submit_desc failed ret=%d\n", __func__, ret);
		goto err;
	}

	ret = check_completion(idxd_desc->iax_completion, true);
	if (ret) {
		pr_warn("%s: check_completion failed ret=%d\n", __func__, ret);
		goto err;
	}

	dma_unmap_single(dev, src_addr, slen, DMA_TO_DEVICE);
	dma_unmap_single(dev, dst_addr, *dlen, DMA_FROM_DEVICE);

	*dlen = idxd_desc->iax_completion->output_size;

	idxd_free_desc(wq, idxd_desc);

	if (!iax_verify_compress)
		goto out;

	compression_crc = idxd_desc->iax_completion->crc;

	/* Update stats */
	update_total_comp_calls();
	update_total_comp_bytes_out(*dlen);
	update_wq_comp_calls(wq);
	update_wq_comp_bytes(wq, *dlen);

	idxd_desc = idxd_alloc_desc(wq, IDXD_OP_BLOCK);
	if (IS_ERR(idxd_desc)) {
		pr_err("%s: idxd descriptor allocation failed\n", __func__);
		pr_warn("%s: iax compress (verify) failed: ret=%ld\n", __func__,
			PTR_ERR(idxd_desc));

		return PTR_ERR(idxd_desc);
	}
	desc = idxd_desc->iax_hw;

	/* Verify (optional) - decompress and check crc, suppress dest write */

	desc->flags = IDXD_OP_FLAG_CRAV | IDXD_OP_FLAG_RCR | IDXD_OP_FLAG_CC;
	desc->opcode = IAX_OPCODE_DECOMPRESS;
	desc->max_dst_size = PAGE_SIZE;
	desc->decompr_flags = IAX_DECOMP_FLAGS | IAX_DECOMP_SUPPRESS_OUTPUT;
#ifdef SPR_E0
	desc->priv = 1;
#else
	desc->priv = 0;
#endif

	src_addr = dma_map_single(dev, (void *)src, slen, DMA_TO_DEVICE);
	pr_debug("%s: dma_map_single, src_addr %llx, dev %p, src %p, slen %d\n", __func__, src_addr, dev, src, slen);
	if (unlikely(dma_mapping_error(dev, src_addr))) {
		pr_debug("%s: dma_map_single err, exiting\n", __func__);
		ret = -ENOMEM;
		goto err_map_src;
	}

	dst_addr = dma_map_single(dev, (void *)dst, *dlen, DMA_FROM_DEVICE);
	pr_debug("%s: dma_map_single, dst_addr %llx, dev %p, dst %p, *dlen %d\n", __func__, dst_addr, dev, dst, *dlen);
	if (unlikely(dma_mapping_error(dev, dst_addr))) {
		pr_debug("%s: dma_map_single err, exiting\n", __func__);
		ret = -ENOMEM;
		goto err_map_dst;
	}

	desc->src1_addr = (u64)dst_addr;
	desc->src1_size = *dlen;
	desc->dst_addr = (u64)src_addr;
	desc->max_dst_size = slen;
	desc->completion_addr = idxd_desc->compl_dma;

	ret = idxd_submit_desc(wq, idxd_desc);
	if (ret) {
		pr_warn("%s: submit_desc (verify) failed ret=%d\n", __func__, ret);
		goto err;
	}

	ret = check_completion(idxd_desc->iax_completion, true);
	if (ret) {
		pr_warn("%s: check_completion (verify) failed ret=%d\n", __func__, ret);
		goto err;
	}

	if (compression_crc != idxd_desc->iax_completion->crc) {
		ret = -EINVAL;
		pr_err("%s: iax comp/decomp crc mismatch: comp=0x%x, decomp=0x%x\n", __func__,
		       compression_crc, idxd_desc->iax_completion->crc);
		print_hex_dump(KERN_INFO, "cmp-rec: ", DUMP_PREFIX_OFFSET, 8, 1, idxd_desc->iax_completion, 64, 0);
		goto err;
	}

	dma_unmap_single(dev, src_addr, slen, DMA_TO_DEVICE);
	dma_unmap_single(dev, dst_addr, *dlen, DMA_FROM_DEVICE);

	idxd_free_desc(wq, idxd_desc);
out:
	return ret;
err:
	dma_unmap_single(dev, dst_addr, *dlen, DMA_FROM_DEVICE);
err_map_dst:
	dma_unmap_single(dev, src_addr, slen, DMA_TO_DEVICE);
err_map_src:
	idxd_free_desc(wq, idxd_desc);
	pr_warn("iax compress failed: ret=%d\n", ret);

	goto out;
}

static int iax_decompress(struct crypto_tfm *tfm,
			  const u8 *src, unsigned int slen,
			  u8 *dst, unsigned int *dlen)
{
	dma_addr_t src_addr, dst_addr;
	struct idxd_desc *idxd_desc;
	struct iax_hw_desc *desc;
	struct idxd_wq *wq;
	struct device *dev;
	int ret = 0;

	wq = *per_cpu_ptr(wq_table, smp_processor_id());
	if (!wq) {
		pr_err("%s: no wq configured for cpu=%d\n", __func__, smp_processor_id());
		return -ENODEV;
	}
	dev = &wq->idxd->pdev->dev;

	pr_debug("%s: using wq for cpu=%d = wq %p\n", __func__, smp_processor_id(), wq);

	idxd_desc = idxd_alloc_desc(wq, IDXD_OP_BLOCK);
	if (IS_ERR(idxd_desc)) {
		pr_err("%s: idxd descriptor allocation failed\n", __func__);
		pr_warn("%s: iax decompress failed: ret=%ld\n", __func__, PTR_ERR(idxd_desc));

		return PTR_ERR(idxd_desc);
	}
	desc = idxd_desc->iax_hw;

	desc->flags = IDXD_OP_FLAG_CRAV | IDXD_OP_FLAG_RCR | IDXD_OP_FLAG_CC;
	desc->opcode = IAX_OPCODE_DECOMPRESS;
	desc->max_dst_size = PAGE_SIZE;
	desc->decompr_flags = IAX_DECOMP_FLAGS;
#ifdef SPR_E0
	desc->priv = 1;
#else
	desc->priv = 0;
#endif

	src_addr = dma_map_single(dev, (void *)src, slen, DMA_TO_DEVICE);
	pr_debug("%s: dma_map_single, src_addr %llx, dev %p, src %p, slen %d\n", __func__, src_addr, dev, src, slen);
	if (unlikely(dma_mapping_error(dev, src_addr))) {
		pr_debug("%s: dma_map_single err, exiting\n", __func__);
		ret = -ENOMEM;
		goto err_map_src;
	}

	dst_addr = dma_map_single(dev, (void *)dst, *dlen, DMA_FROM_DEVICE);
	pr_debug("%s: dma_map_single, dst_addr %llx, dev %p, dst %p, *dlen %d\n", __func__, dst_addr, dev, dst, *dlen);
	if (unlikely(dma_mapping_error(dev, dst_addr))) {
		pr_debug("%s: dma_map_single err, exiting\n", __func__);
		ret = -ENOMEM;
		goto err_map_dst;
	}

	desc->src1_addr = (u64)src_addr;
	desc->dst_addr = (u64)dst_addr;
	desc->max_dst_size = *dlen;
	desc->src1_size = slen;
	desc->completion_addr = idxd_desc->compl_dma;

	ret = idxd_submit_desc(wq, idxd_desc);
	if (ret) {
		pr_warn("%s: submit_desc failed ret=%d\n", __func__, ret);
		goto err;
	}

	ret = check_completion(idxd_desc->iax_completion, true);
	if (ret) {
		pr_warn("%s: check_completion failed ret=%d\n", __func__, ret);
		goto err;
	}

	dma_unmap_single(dev, src_addr, slen, DMA_TO_DEVICE);
	dma_unmap_single(dev, dst_addr, *dlen, DMA_FROM_DEVICE);

	*dlen = idxd_desc->iax_completion->output_size;

	idxd_free_desc(wq, idxd_desc);

	/* Update stats */
	update_total_decomp_calls();
	update_total_decomp_bytes_in(slen);
	update_wq_decomp_calls(wq);
	update_wq_decomp_bytes(wq, slen);
out:
	return ret;
err:
	dma_unmap_single(dev, dst_addr, *dlen, DMA_FROM_DEVICE);
err_map_dst:
	dma_unmap_single(dev, src_addr, slen, DMA_TO_DEVICE);
err_map_src:
	idxd_free_desc(wq, idxd_desc);
	pr_warn("iax decompress failed: ret=%d\n", ret);

	goto out;
}

static int iax_comp_compress(struct crypto_tfm *tfm,
			     const u8 *src, unsigned int slen,
			     u8 *dst, unsigned int *dlen)
{
	u64 start_time_ns;
	int ret = 0;

	if (!iax_crypto_enabled) {
		pr_debug("%s: iax_crypto disabled, using deflate-generic compression\n", __func__);
		ret = crypto_comp_compress(deflate_generic_tfm,
					   src, slen, dst, dlen);
		return ret;
	}

	pr_debug("%s: src %p, slen %d, dst %p, dlen %u\n",
		 __func__, src, slen, dst, *dlen);

	start_time_ns = ktime_get_ns();
	ret = iax_compress(tfm, src, slen, dst, dlen);
	update_max_comp_delay_ns(start_time_ns);
	if (ret != 0)
		pr_warn("synchronous compress failed ret=%d\n", ret);

	return ret;
}

static int iax_comp_decompress(struct crypto_tfm *tfm,
			       const u8 *src, unsigned int slen,
			       u8 *dst, unsigned int *dlen)
{
	u64 start_time_ns;
	int ret = 0;

	if (!iax_crypto_enabled) {
		pr_debug("%s: iax_crypto disabled, using deflate-generic decompression\n", __func__);
		ret = crypto_comp_decompress(deflate_generic_tfm,
					     src, slen, dst, dlen);
		return ret;
	}

	pr_debug("%s: src %p, slen %d, dst %p, dlen %u\n",
		 __func__, src, slen, dst, *dlen);

	start_time_ns = ktime_get_ns();
	ret = iax_decompress(tfm, src, slen, dst, dlen);
	update_max_decomp_delay_ns(start_time_ns);
	if (ret != 0)
		pr_warn("synchronous decompress failed ret=%d\n", ret);

	return ret;
}

static struct crypto_alg iax_comp_deflate = {
	.cra_name		= "deflate",
	.cra_driver_name	= "iax_crypto",
	.cra_flags		= CRYPTO_ALG_TYPE_COMPRESS,
	.cra_priority		= IAX_ALG_PRIORITY,
	.cra_module		= THIS_MODULE,
	.cra_u			= {
		.compress = {
			.coa_compress	= iax_comp_compress,
			.coa_decompress	= iax_comp_decompress
		}
	}
};

static int iax_comp_acompress(struct acomp_req *req)
{
	struct crypto_tfm *tfm = req->base.tfm;
	u64 start_time_ns;
	void *src, *dst;
	int ret = 0;

	src = kmap_atomic(sg_page(req->src)) + req->src->offset;
	dst = kmap_atomic(sg_page(req->dst)) + req->dst->offset;

	if (!iax_crypto_enabled) {
		pr_debug("%s: iax_crypto disabled, using deflate-generic compression\n", __func__);
		ret = crypto_comp_compress(deflate_generic_tfm,
					   src, req->slen, dst, &req->dlen);
		kunmap_atomic(src);
		kunmap_atomic(dst);

		return ret;
	}

	pr_debug("%s: src %p (offset %d), slen %d, dst %p (offset %d), dlen %u\n",
		 __func__, src, req->src->offset, req->slen,
		 dst, req->dst->offset, req->dlen);

	start_time_ns = ktime_get_ns();
	ret = iax_compress(tfm, (const u8 *)src, req->slen, (u8 *)dst, &req->dlen);
	update_max_acomp_delay_ns(start_time_ns);

	kunmap_atomic(src);
	kunmap_atomic(dst);

	if (ret != 0)
		pr_warn("asynchronous compress failed ret=%d\n", ret);

	return ret;
}

static int iax_comp_adecompress(struct acomp_req *req)
{
	struct crypto_tfm *tfm = req->base.tfm;
	u64 start_time_ns;
	void *src, *dst;
	int ret;

	src = kmap_atomic(sg_page(req->src)) + req->src->offset;
	dst = kmap_atomic(sg_page(req->dst)) + req->dst->offset;

	if (!iax_crypto_enabled) {
		pr_debug("%s: iax_crypto disabled, using deflate-generic decompression\n", __func__);
		ret = crypto_comp_decompress(deflate_generic_tfm,
					     src, req->slen, dst, &req->dlen);
		kunmap_atomic(src);
		kunmap_atomic(dst);
		return ret;
	}

	pr_debug("%s: src %p (offset %d), slen %d, dst %p (offset %d), dlen %u\n",
		 __func__, src, req->src->offset, req->slen,
		 dst, req->dst->offset, req->dlen);

	start_time_ns = ktime_get_ns();
	ret = iax_decompress(tfm, (const u8 *)src, req->slen, (u8 *)dst, &req->dlen);
	update_max_decomp_delay_ns(start_time_ns);

	kunmap_atomic(src);
	kunmap_atomic(dst);

	if (ret != 0)
		pr_warn("asynchronous decompress failed ret=%d\n", ret);

	return ret;
}

static struct acomp_alg iax_acomp_deflate = {
	.compress		= iax_comp_acompress,
	.decompress		= iax_comp_adecompress,
	.base			= {
		.cra_name		= "deflate",
		.cra_driver_name	= "iax_crypto",
		.cra_module		= THIS_MODULE,
		.cra_priority           = IAX_ALG_PRIORITY,
	}
};

static int iax_register_compression_device(void)
{
	int ret;

	ret = crypto_register_alg(&iax_comp_deflate);
	if (ret < 0) {
		pr_err("deflate algorithm registration failed\n");
		return ret;
	}

	ret = crypto_register_acomp(&iax_acomp_deflate);
	if (ret) {
		pr_err("deflate algorithm acomp registration failed (%d)\n", ret);
		goto err_unregister_alg_deflate;
	}

	return ret;

err_unregister_alg_deflate:
	crypto_unregister_alg(&iax_comp_deflate);

	return ret;
}

static void iax_unregister_compression_device(void)
{
	crypto_unregister_alg(&iax_comp_deflate);
	crypto_unregister_acomp(&iax_acomp_deflate);
}

static void rebalance_wq_table(void)
{
	int node, cpu, iax;
	struct idxd_wq *wq;

	if (nr_iax == 0)
		return;

	pr_debug("%s: nr_nodes=%d, nr_cpus %d, nr_iax %d, cpus_per_iax %d\n",
		 __func__, nr_nodes, nr_cpus, nr_iax, cpus_per_iax);

	for (cpu = 0; cpu < nr_cpus; cpu++) {
		iax = cpu_to_iax(cpu);
		pr_debug("%s: iax=%d\n", __func__, iax);

		BUG_ON(iax == -1);

		wq = request_iax_wq(iax);
		if (!wq) {
			pr_err("could not get wq for iax %d!\n", iax);
			return;
		}

		*per_cpu_ptr(wq_table, cpu) = wq;
		pr_debug("%s: assigned wq for cpu=%d, node=%d = wq %p\n", __func__, cpu, node, wq);
	}
}

static int iax_crypto_probe(struct idxd_dev *idxd_dev)
{
	struct idxd_wq *wq = idxd_dev_to_wq(idxd_dev);
	struct idxd_device *idxd = wq->idxd;
	struct idxd_driver_data *data = idxd->data;
	struct device *dev = &idxd_dev->conf_dev;
	int ret = 0;

	if (idxd->state != IDXD_DEV_ENABLED)
		return -ENXIO;

	if (data->type != IDXD_TYPE_IAX)
		return -ENODEV;

	mutex_lock(&wq->wq_lock);

	if (!idxd_wq_driver_name_match(wq, dev)) {
		pr_warn("%s: wq driver_name match failed: wq driver_name %s, dev driver name %s\n", __func__, wq->driver_name, dev->driver->name);
		idxd->cmd_status = IDXD_SCMD_WQ_NO_DRV_NAME;
		ret = -ENODEV;
		goto err;
	}

	wq->type = IDXD_WQT_KERNEL;

	ret = __drv_enable_wq(wq);
	if (ret < 0) {
		pr_warn("%s: enable wq %d failed: %d\n", __func__, wq->id, ret);
		ret = -ENXIO;
		goto err;
	}

	ret = idxd_wq_init_percpu_ref(wq);
	if (ret < 0) {
		idxd->cmd_status = IDXD_SCMD_PERCPU_ERR;
		pr_warn("%s: WQ percpu_ref setup failed: ret=%d\n", __func__, ret);
		goto err_ref;
	}

	ret = save_iax_wq(wq);
	if (ret)
		goto err_save;

	rebalance_wq_table();
out:
	mutex_unlock(&wq->wq_lock);

	return ret;

err_save:
	__idxd_wq_quiesce(wq);
	percpu_ref_exit(&wq->wq_active);
err_ref:
	idxd_wq_free_resources(wq);
	__drv_disable_wq(wq);
err:
	wq->type = IDXD_WQT_NONE;

	goto out;
}

static void iax_crypto_remove(struct idxd_dev *idxd_dev)
{
	struct idxd_wq *wq = idxd_dev_to_wq(idxd_dev);

	mutex_lock(&wq->wq_lock);

	__idxd_wq_quiesce(wq);
	remove_iax_wq(wq);
	__drv_disable_wq(wq);
	idxd_wq_free_resources(wq);
	wq->type = IDXD_WQT_NONE;
	percpu_ref_exit(&wq->wq_active);
	rebalance_wq_table();

	mutex_unlock(&wq->wq_lock);
}

static enum idxd_dev_type dev_types[] = {
	IDXD_DEV_WQ,
	IDXD_DEV_NONE,
};

static struct idxd_device_driver iax_crypto_driver = {
	.probe = iax_crypto_probe,
	.remove = iax_crypto_remove,
	.name = "crypto",
	.type = dev_types,
};

static int __init iax_crypto_init_module(void)
{
	int ret = 0;

	nr_cpus = num_online_cpus();
	nr_nodes = num_online_nodes();

	if (crypto_has_comp("deflate-generic", 0, 0))
		deflate_generic_tfm = crypto_alloc_comp("deflate-generic", 0, 0);

	if (IS_ERR_OR_NULL(deflate_generic_tfm)) {
		pr_err("IAX could not alloc %s tfm: errcode = %ld\n",
		       "deflate-generic", PTR_ERR(deflate_generic_tfm));
		return -ENOMEM;
	}

	wq_table = alloc_percpu(struct idxd_wq *);
	if (!wq_table)
		return -ENOMEM;

	ret = __idxd_driver_register(&iax_crypto_driver, THIS_MODULE,
				     KBUILD_MODNAME);
	if (ret) {
		pr_err("IAX wq sub-driver registration failed\n");
		goto err_driver_register;
	}

	ret = iax_register_compression_device();
	if (ret < 0) {
		pr_err("IAX compression device registration failed\n");
		goto err_crypto_register;
	}

	if (iax_crypto_debugfs_init())
		pr_warn("debugfs init failed, stats not available\n");

	pr_info("%s: initialized\n", __func__);
out:
	return ret;

err_crypto_register:
	idxd_driver_unregister(&iax_crypto_driver);
err_driver_register:
	crypto_free_comp(deflate_generic_tfm);
	free_percpu(wq_table);

	goto out;
}

static void __exit iax_crypto_cleanup_module(void)
{
	iax_crypto_debugfs_cleanup();
	idxd_driver_unregister(&iax_crypto_driver);
	iax_unregister_compression_device();
	free_percpu(wq_table);
	free_iax_devices();
	crypto_free_comp(deflate_generic_tfm);
	pr_info("%s: cleaned up\n", __func__);
}

MODULE_IMPORT_NS(IDXD);
MODULE_LICENSE("GPL v2");
MODULE_ALIAS_IDXD_DEVICE(0);
MODULE_AUTHOR("Intel Corporation");
MODULE_DESCRIPTION("IAX Crypto Driver");

module_init(iax_crypto_init_module);
module_exit(iax_crypto_cleanup_module);
