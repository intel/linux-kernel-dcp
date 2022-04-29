// SPDX-License-Identifier: GPL-2.0
/* Copyright(c) 2019,2020 Intel Corporation. All rights rsvd. */
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/pci.h>
#include <linux/device.h>
#include <linux/sched/task.h>
#include <linux/io-64-nonatomic-lo-hi.h>
#include <linux/mm.h>
#include <linux/mmu_context.h>
#include <linux/vfio.h>
#include <linux/mdev.h>
#include <linux/msi.h>
#include <linux/intel-iommu.h>
#include <linux/intel-svm.h>
#include <linux/kvm_host.h>
#include <linux/eventfd.h>
#include <linux/irqchip/irq-ims-msi.h>
#include <uapi/linux/idxd.h>
#include "registers.h"
#include "idxd.h"
#include "../mdev_private.h"
#include "mdev.h"

static void vidxd_do_command(struct vdcm_idxd *vidxd, u32 val);

void vidxd_send_interrupt(struct vdcm_idxd *vidxd, int msix_idx)
{
	struct vfio_pci_core_device *vfio_pdev = &vidxd->vfio_pdev;

	eventfd_signal(vfio_pdev->ctx[msix_idx].trigger, 1);
}

static void vidxd_report_error(struct vdcm_idxd *vidxd, unsigned int error)
{
	u8 *bar0 = vidxd->bar0;
	union sw_err_reg *swerr = (union sw_err_reg *)(bar0 + IDXD_SWERR_OFFSET);
	union genctrl_reg *genctrl;
	bool send = false;

	if (!swerr->valid) {
		memset(swerr, 0, sizeof(*swerr));
		swerr->valid = 1;
		swerr->error = error;
		send = true;
	} else if (swerr->valid && !swerr->overflow) {
		swerr->overflow = 1;
	}

	genctrl = (union genctrl_reg *)(bar0 + IDXD_GENCTRL_OFFSET);
	if (send && genctrl->softerr_int_en) {
		u32 *intcause = (u32 *)(bar0 + IDXD_INTCAUSE_OFFSET);

		*intcause |= IDXD_INTC_ERR;
		vidxd_send_interrupt(vidxd, 0);
	}
}

void vidxd_notify_revoked_handles (struct vdcm_idxd *vidxd)
{
	u8 *bar0 = vidxd->bar0;
	u32 *intcause = (u32 *)(bar0 + IDXD_INTCAUSE_OFFSET);

	*intcause |= IDXD_INTC_INT_HANDLE_REVOKED;
	pr_info("informating guest about revoked handles\n");
	vidxd_send_interrupt(vidxd, 0);
}

static int vidxd_set_ims_pasid(struct vdcm_idxd *vidxd, int index, bool pasid_en, u32 gpasid)
{
	struct device *dev = mdev_dev(vidxd->ivdev.mdev);
	u64 auxval;
	u32 pasid;
	int irq;
	int rc;

	irq = dev_msi_irq_vector(dev, index);

	if (pasid_en)
		rc = idxd_mdev_get_host_pasid(vidxd->ivdev.mdev, gpasid, &pasid);
	else
		rc = idxd_mdev_get_pasid(vidxd->ivdev.mdev, &pasid);
	if (rc < 0)
		return rc;
	dev_dbg(dev, "IMS entry: %d pasid_en: %u guest pasid %u host pasid: %u\n",
		index, pasid_en, gpasid, pasid);
	auxval = ims_ctrl_pasid_aux(pasid, 1);
	return irq_set_auxdata(irq, IMS_AUXDATA_CONTROL_WORD, auxval);

}

int vidxd_mmio_write(struct vdcm_idxd *vidxd, u64 pos, void *buf, unsigned int size)
{
	u32 offset = pos & (vidxd->bar_size[0] - 1);
	u8 *bar0 = vidxd->bar0;
	struct device *dev = mdev_dev(vidxd->ivdev.mdev);

	dev_dbg(dev, "vidxd mmio W %d %x %x: %llx\n", vidxd->wq->id, size,
		offset, get_reg_val(buf, size));

	if (((size & (size - 1)) != 0) || (offset & (size - 1)) != 0) {
		dev_warn(dev, "XXX %s out of bounds\n", __func__);
		return -EINVAL;
	}

	/* If we don't limit this, we potentially can write out of bound */
	if (size > sizeof(u32)) {
		dev_warn(dev, "XXX %s size greater than u32\n", __func__);
		return -EINVAL;
	}

	switch (offset) {
	case IDXD_GENCFG_OFFSET ... IDXD_GENCFG_OFFSET + 3:
		/* Write only when device is disabled. */
		if (vidxd_state(vidxd) == IDXD_DEVICE_STATE_DISABLED)
			memcpy(bar0 + offset, buf, size);
		break;

	case IDXD_GENCTRL_OFFSET:
		memcpy(bar0 + offset, buf, size);
		break;

	case IDXD_INTCAUSE_OFFSET:
		*(u32 *)&bar0[offset] &= ~(get_reg_val(buf, 4));
		break;

	case IDXD_CMD_OFFSET: {
		u32 *cmdsts = (u32 *)(bar0 + IDXD_CMDSTS_OFFSET);
		u32 val = get_reg_val(buf, size);

		if (size != sizeof(u32))
			return -EINVAL;

		/* Check and set command in progress */
		if (test_and_set_bit(IDXD_CMDS_ACTIVE_BIT, (unsigned long *)cmdsts) == 0)
			vidxd_do_command(vidxd, val);
		else
			vidxd_report_error(vidxd, DSA_ERR_CMD_REG);
		break;
	}

	case IDXD_SWERR_OFFSET:
		/* W1C */
		bar0[offset] &= ~(get_reg_val(buf, 1) & GENMASK(1, 0));
		break;

	case VIDXD_WQCFG_OFFSET ... VIDXD_WQCFG_OFFSET + VIDXD_WQ_CTRL_SZ - 1: {
		union wqcfg *wqcfg;
		int wq_id = (offset - VIDXD_WQCFG_OFFSET) / 0x20;
		int subreg = offset & 0x1c;
		u32 new_val;

		if (wq_id >= VIDXD_MAX_WQS)
			break;

		/* FIXME: Need to sanitize for RO Config WQ mode 1 */
		wqcfg = (union wqcfg *)(bar0 + VIDXD_WQCFG_OFFSET + wq_id * 0x20);
		if (size >= 4) {
			new_val = get_reg_val(buf, 4);
		} else {
			u32 tmp1, tmp2, shift, mask;

			switch (subreg) {
			case 4:
				tmp1 = wqcfg->bits[1];
				break;
			case 8:
				tmp1 = wqcfg->bits[2];
				break;
			case 12:
				tmp1 = wqcfg->bits[3];
				break;
			case 16:
				tmp1 = wqcfg->bits[4];
				break;
			case 20:
				tmp1 = wqcfg->bits[5];
				break;
			default:
				tmp1 = 0;
			}

			tmp2 = get_reg_val(buf, size);
			shift = (offset & 0x03U) * 8;
			mask = ((1U << size * 8) - 1u) << shift;
			new_val = (tmp1 & ~mask) | (tmp2 << shift);
		}

		if (subreg == 8) {
			if (wqcfg->wq_state == 0) {
				wqcfg->bits[2] &= 0xfe;
				wqcfg->bits[2] |= new_val & 0xffffff01;
			}
		}

		break;
	} /* WQCFG */

	case VIDXD_GRPCFG_OFFSET ...  VIDXD_GRPCFG_OFFSET + VIDXD_GRP_CTRL_SZ - 1:
		/* Nothing is written. Should be all RO */
		break;

	case VIDXD_MSIX_TABLE_OFFSET ...  VIDXD_MSIX_TABLE_OFFSET + VIDXD_MSIX_TBL_SZ - 1: {
		int index = (offset - VIDXD_MSIX_TABLE_OFFSET) / 0x10;
		u8 *msix_entry = &bar0[VIDXD_MSIX_TABLE_OFFSET + index * 0x10];
		u64 *pba = (u64 *)(bar0 + VIDXD_MSIX_PBA_OFFSET);
		u8 ctrl;

		ctrl = msix_entry[MSIX_ENTRY_CTRL_BYTE];
		memcpy(bar0 + offset, buf, size);
		/* Handle clearing of UNMASK bit */
		if (!(msix_entry[MSIX_ENTRY_CTRL_BYTE] & MSIX_ENTRY_MASK_INT) &&
		    ctrl & MSIX_ENTRY_MASK_INT)
			if (test_and_clear_bit(index, (unsigned long *)pba))
				vidxd_send_interrupt(vidxd, index);
		break;
	}

	case VIDXD_MSIX_PERM_OFFSET ...  VIDXD_MSIX_PERM_OFFSET + VIDXD_MSIX_PERM_TBL_SZ - 1: {
		int index;
		u32 msix_perm;

		if (size != sizeof(u32) || !IS_ALIGNED(offset, sizeof(u64))) {
			dev_warn(dev, "XXX unaligned MSIX PERM access\n");
			break;
		}

		index = (offset - VIDXD_MSIX_PERM_OFFSET) / 8;
		msix_perm = get_reg_val(buf, sizeof(u32)) & 0xfffff00d;
		memcpy(bar0 + offset, buf, size);
		dev_dbg(dev, "%s writing to MSIX_PERM: %#x offset %#x index: %u\n",
			__func__, msix_perm, offset, index);
		break;
	}
	} /* offset */

	return 0;
}

int vidxd_portal_mmio_read(struct vdcm_idxd *vidxd, u64 pos, void *buf,
                                unsigned int size)
{
	u32 offset = pos & (vidxd->bar_size[1] - 1);
	struct device *dev = mdev_dev(vidxd->ivdev.mdev);

	BUG_ON((size & (size - 1)) != 0);
	BUG_ON(size > 8);
	BUG_ON((offset & (size - 1)) != 0);

	memset(buf, 0xff, size);

	dev_dbg(dev, "vidxd portal mmio R %d %x %x: %llx\n",
		vidxd->wq->id, size, offset, get_reg_val(buf, size));
	return 0;
}

int vidxd_portal_mmio_write(struct vdcm_idxd *vidxd, u64 pos, void *buf,
				unsigned int size)
{
	struct device *dev = mdev_dev(vidxd->ivdev.mdev);
	u32 offset = pos & (vidxd->bar_size[1] - 1);
	uint16_t wq_id = offset >> 14;
	uint16_t portal_id, portal_offset;
	struct idxd_virtual_wq *vwq;
	struct idxd_wq *wq;
	struct idxd_wq_portal *portal;
	enum idxd_portal_prot portal_prot = IDXD_PORTAL_UNLIMITED;
	int rc = 0;

	BUG_ON((size & (size - 1)) != 0);
	BUG_ON(size > 64);
	BUG_ON((offset & (size - 1)) != 0);

	dev_dbg(dev, "vidxd portal mmio W %d %x %x: %llx\n", vidxd->wq->id, size,
			offset, get_reg_val(buf, size));

	if (wq_id >= vidxd->num_wqs) {
		printk("DSA portal write: Invalid wq  %d\n", wq_id);
	}

	vwq = &vidxd->vwq;
	wq = vidxd->wq;

	if (!wq_dedicated(wq) || (((offset >> PAGE_SHIFT) & 0x3) == 1))
		portal_prot = IDXD_PORTAL_LIMITED;

	portal_id = (offset & 0xFFF) >> 6;
	portal_offset = offset & 0x3F;

	portal = &vwq->portals[portal_id];

	portal->count += size;
	memcpy(&portal->data[portal_offset], buf, size);

	if (portal->count == IDXD_DESC_SIZE) {
		struct idxd_wq_desc_elem *elem;
		u64 *p = (u64 *)portal->data;
		printk("desc: %016llx %016llx  %016llx %016llx %016llx %016llx %016llx %016llx\n",
				p[0], p[1], p[2], p[3], p[4], p[5], p[6], p[7]);

		mutex_lock(&vidxd->mig_submit_lock);
		if (vidxd->paused) {
#if 0
			if (wq_dedicated(wq)) {
#endif
				/* Queue the descriptor if submitted to DWQ */
				if (vwq->ndescs == wq->size) {
					printk("can't submit more descriptors than WQ size. Dropping.\n");
					goto out_unlock;
				}

				elem = kmalloc(sizeof(struct idxd_wq_desc_elem),
					GFP_KERNEL);

				if (elem == NULL) {
					printk("kmalloc failed\n");
					rc = -ENOMEM;
					goto out_unlock;
				}
				printk("queuing the desc\n");
				memcpy(elem->work_desc, portal->data, IDXD_DESC_SIZE);
				elem->portal_prot = portal_prot;
				elem->portal_id = portal_id;

				list_add_tail(&elem->link, &vwq->head);
				vwq->ndescs++;
#if 0
			} else {
				/* Return retry if submitted to SWQ */
				rc = -EAGAIN;
				goto out_unlock;
			}
#endif
               } else {
			void __iomem *wq_portal;
			wq_portal = vidxd->idxd->portal_base +
				idxd_get_wq_portal_full_offset(wq->id,
						portal_prot, IDXD_IRQ_IMS);
			wq_portal += (portal_id << 6);
			printk("submitting a desc to WQ %d ded %d\n", wq->id,
					wq_dedicated(wq));
			if (wq_dedicated(wq)) {
				iosubmit_cmds512(wq_portal, (struct dsa_hw_desc *)p, 1);
			} else {
				int rc;
				struct dsa_hw_desc *hw =
					(struct dsa_hw_desc *)portal->data;
				int hpasid, gpasid = hw->pasid;

				/* Translate the gpasid in the descriptor */
				rc = idxd_mdev_get_host_pasid(vidxd->ivdev.mdev,
							gpasid, &hpasid);
                                if (rc < 0) {
                                        pr_info("gpasid->hpasid trans failed\n");
					rc = -EINVAL;
					goto out_unlock;
                                }
                                hw->pasid = hpasid;

				/* FIXME: Allow enqcmds to retry a few times
				 * before failing */
				rc = enqcmds(wq_portal, hw);
				if (rc < 0) {
					pr_info("%s: enqcmds failed\n", __func__);
					goto out_unlock;
				}
			}
		}
out_unlock:
		mutex_unlock(&vidxd->mig_submit_lock);
		memset(&portal->data, 0, IDXD_DESC_SIZE);
		portal->count = 0;
	}

	return rc;
}

int vidxd_mmio_read(struct vdcm_idxd *vidxd, u64 pos, void *buf, unsigned int size)
{
	u32 offset = pos & (vidxd->bar_size[0] - 1);
	struct device *dev = mdev_dev(vidxd->ivdev.mdev);

	memcpy(buf, vidxd->bar0 + offset, size);

	dev_dbg(dev, "vidxd mmio R %d %x %x: %llx\n",
		vidxd->wq->id, size, offset, get_reg_val(buf, size));
	return 0;
}

int vidxd_cfg_read(struct vdcm_idxd *vidxd, unsigned int pos, void *buf, unsigned int count)
{
	u32 offset = pos & 0xfff;
	struct device *dev = mdev_dev(vidxd->ivdev.mdev);

	memcpy(buf, &vidxd->cfg[offset], count);

	dev_dbg(dev, "vidxd pci R %d %x %x: %llx\n",
		vidxd->wq->id, count, offset, get_reg_val(buf, count));

	return 0;
}

/*
 * Much of the emulation code has been borrowed from Intel i915 cfg space
 * emulation code.
 * drivers/gpu/drm/i915/gvt/cfg_space.c:
 */

/*
 * Bitmap for writable bits (RW or RW1C bits, but cannot co-exist in one
 * byte) byte by byte in standard pci configuration space. (not the full
 * 256 bytes.)
 */
static const u8 pci_cfg_space_rw_bmp[PCI_INTERRUPT_LINE + 4] = {
	[PCI_COMMAND]		= 0xff, 0x07,
	[PCI_STATUS]		= 0x00, 0xf9, /* the only one RW1C byte */
	[PCI_CACHE_LINE_SIZE]	= 0xff,
	[PCI_BASE_ADDRESS_0 ... PCI_CARDBUS_CIS - 1] = 0xff,
	[PCI_ROM_ADDRESS]	= 0x01, 0xf8, 0xff, 0xff,
	[PCI_INTERRUPT_LINE]	= 0xff,
};

static void _pci_cfg_mem_write(struct vdcm_idxd *vidxd, unsigned int off, u8 *src,
			       unsigned int bytes)
{
	u8 *cfg_base = vidxd->cfg;
	u8 mask, new, old;
	int i = 0;

	for (; i < bytes && (off + i < sizeof(pci_cfg_space_rw_bmp)); i++) {
		mask = pci_cfg_space_rw_bmp[off + i];
		old = cfg_base[off + i];
		new = src[i] & mask;

		/**
		 * The PCI_STATUS high byte has RW1C bits, here
		 * emulates clear by writing 1 for these bits.
		 * Writing a 0b to RW1C bits has no effect.
		 */
		if (off + i == PCI_STATUS + 1)
			new = (~new & old) & mask;

		cfg_base[off + i] = (old & ~mask) | new;
	}

	/* For other configuration space directly copy as it is. */
	if (i < bytes)
		memcpy(cfg_base + off + i, src + i, bytes - i);
}

static inline void _write_pci_bar(struct vdcm_idxd *vidxd, u32 offset, u32 val, bool low)
{
	u32 *pval;

	/* BAR offset should be 32 bits algiend */
	offset = rounddown(offset, 4);
	pval = (u32 *)(vidxd->cfg + offset);

	if (low) {
		/*
		 * only update bit 31 - bit 4,
		 * leave the bit 3 - bit 0 unchanged.
		 */
		*pval = (val & GENMASK(31, 4)) | (*pval & GENMASK(3, 0));
	} else {
		*pval = val;
	}
}

static int _pci_cfg_bar_write(struct vdcm_idxd *vidxd, unsigned int offset, void *p_data,
			      unsigned int bytes)
{
	u32 new = *(u32 *)(p_data);
	bool lo = IS_ALIGNED(offset, 8);
	u64 size;
	unsigned int bar_id;

	/*
	 * Power-up software can determine how much address
	 * space the device requires by writing a value of
	 * all 1's to the register and then reading the value
	 * back. The device will return 0's in all don't-care
	 * address bits.
	 */
	if (new == 0xffffffff) {
		switch (offset) {
		case PCI_BASE_ADDRESS_0:
		case PCI_BASE_ADDRESS_1:
		case PCI_BASE_ADDRESS_2:
		case PCI_BASE_ADDRESS_3:
			bar_id = (offset - PCI_BASE_ADDRESS_0) / 8;
			size = vidxd->bar_size[bar_id];
			_write_pci_bar(vidxd, offset, size >> (lo ? 0 : 32), lo);
			break;
		default:
			/* Unimplemented BARs */
			_write_pci_bar(vidxd, offset, 0x0, false);
		}
	} else {
		switch (offset) {
		case PCI_BASE_ADDRESS_0:
		case PCI_BASE_ADDRESS_1:
		case PCI_BASE_ADDRESS_2:
		case PCI_BASE_ADDRESS_3:
			_write_pci_bar(vidxd, offset, new, lo);
			break;
		default:
			break;
		}
	}
	return 0;
}

int vidxd_cfg_write(struct vdcm_idxd *vidxd, unsigned int pos, void *buf, unsigned int size)
{
	struct device *dev = &vidxd->idxd->pdev->dev;
	u8 *cfg = vidxd->cfg;
	u32 offset = pos & 0xfff;
	u64 val;

	if (size > 4)
		return -EINVAL;

	if (pos + size > VIDXD_MAX_CFG_SPACE_SZ)
		return -EINVAL;

	dev_dbg(dev, "vidxd pci W %d %x %x: %llx\n", vidxd->wq->id, size, pos,
		get_reg_val(buf, size));

	/* First check if it's PCI_COMMAND */
	if (IS_ALIGNED(pos, 2) && pos == PCI_COMMAND) {
		bool new_bme;
		bool bme;

		if (size > 2)
			return -EINVAL;

		new_bme = !!(get_reg_val(buf, 2) & PCI_COMMAND_MASTER);
		bme = !!(vidxd->cfg[pos] & PCI_COMMAND_MASTER);
		_pci_cfg_mem_write(vidxd, pos, buf, size);

		/* Flag error if turning off BME while device is enabled */
		if ((bme && !new_bme) && vidxd_state(vidxd) == IDXD_DEVICE_STATE_ENABLED)
			vidxd_report_error(vidxd, DSA_ERR_PCI_CFG);
		return 0;
	}

	switch (pos) {
	case PCI_BASE_ADDRESS_0 ... PCI_BASE_ADDRESS_5:
		if (!IS_ALIGNED(pos, 4))
			return -EINVAL;
		return _pci_cfg_bar_write(vidxd, pos, buf, size);

	case VIDXD_ATS_OFFSET + 4:
		if (size < 4)
			break;
		offset += 2;
		buf = buf + 2;
		size -= 2;
		fallthrough;

	case VIDXD_ATS_OFFSET + 6:
		memcpy(&cfg[offset], buf, size);
		break;

	case VIDXD_PRS_OFFSET + 4: {
		u8 old_val, new_val;

		val = get_reg_val(buf, 1);
		old_val = cfg[VIDXD_PRS_OFFSET + 4];
		new_val = val & 1;

		cfg[offset] = new_val;
		if (old_val == 0 && new_val == 1) {
			/*
			 * Clear Stopped, Response Failure,
			 * and Unexpected Response.
			 */
			*(u16 *)&cfg[VIDXD_PRS_OFFSET + 6] &= ~(u16)(0x0103);
		}

		if (size < 4)
			break;

		offset += 2;
		buf = (u8 *)buf + 2;
		size -= 2;
		fallthrough;
	}

	case VIDXD_PRS_OFFSET + 6:
		cfg[offset] &= ~(get_reg_val(buf, 1) & 3);
		break;

	case VIDXD_PRS_OFFSET + 12 ... VIDXD_PRS_OFFSET + 15:
		memcpy(&cfg[offset], buf, size);
		break;

	case VIDXD_PASID_OFFSET + 4:
		if (size < 4)
			break;
		offset += 2;
		buf = buf + 2;
		size -= 2;
		fallthrough;

	case VIDXD_PASID_OFFSET + 6:
		cfg[offset] = get_reg_val(buf, 1) & 5;
		break;

	default:
		_pci_cfg_mem_write(vidxd, pos, buf, size);
	}
	return 0;
}

static void vidxd_mmio_init_grpcap(struct vdcm_idxd *vidxd)
{
	u8 *bar0 = vidxd->bar0;
	union group_cap_reg *grp_cap = (union group_cap_reg *)(bar0 + IDXD_GRPCAP_OFFSET);

	/* single group for current implementation */
	grp_cap->token_en = 0;
	grp_cap->token_limit = 0;
	grp_cap->total_tokens = 0;
	grp_cap->num_groups = 1;
}

static void vidxd_mmio_init_grpcfg(struct vdcm_idxd *vidxd)
{
	u8 *bar0 = vidxd->bar0;
	struct grpcfg *grpcfg = (struct grpcfg *)(bar0 + VIDXD_GRPCFG_OFFSET);
	struct idxd_wq *wq = vidxd->wq;
	struct idxd_group *group = wq->group;
	int i;

	/*
	 * At this point, we are only exporting a single workqueue for
	 * each mdev. So we need to just fake it as first workqueue
	 * and also mark the available engines in this group.
	 */

	/* Set single workqueue and the first one */
	grpcfg->wqs[0] = BIT(0);
	grpcfg->engines = 0;
	for (i = 0; i < group->num_engines; i++)
		grpcfg->engines |= BIT(i);
	grpcfg->flags.bits = group->grpcfg.flags.bits;
}

static void vidxd_mmio_init_wqcap(struct vdcm_idxd *vidxd)
{
	u8 *bar0 = vidxd->bar0;
	struct idxd_wq *wq = vidxd->wq;
	union wq_cap_reg *wq_cap = (union wq_cap_reg *)(bar0 + IDXD_WQCAP_OFFSET);

	wq_cap->occupancy_int = 0;
	wq_cap->occupancy = 0;
	wq_cap->priority = 0;
	wq_cap->total_wq_size = wq->size;
	wq_cap->num_wqs = VIDXD_MAX_WQS;
	wq_cap->wq_ats_support = 0;
	if (wq_dedicated(wq))
		wq_cap->dedicated_mode = 1;
	else
		wq_cap->shared_mode = 1;
}

static void vidxd_mmio_init_wqcfg(struct vdcm_idxd *vidxd)
{
	struct idxd_device *idxd = vidxd->idxd;
	struct idxd_wq *wq = vidxd->wq;
	u8 *bar0 = vidxd->bar0;
	union wqcfg *wqcfg = (union wqcfg *)(bar0 + VIDXD_WQCFG_OFFSET);

	wqcfg->wq_size = wq->size;
	wqcfg->wq_thresh = wq->threshold;

	if (wq_dedicated(wq))
		wqcfg->mode = WQCFG_MODE_DEDICATED;
	else if (device_user_pasid_enabled(idxd))
		wqcfg->pasid_en = 1;

	wqcfg->bof = wq->wqcfg->bof;

	wqcfg->priority = wq->priority;
	wqcfg->max_xfer_shift = ilog2(wq->max_xfer_bytes);
	wqcfg->max_batch_shift = ilog2(wq->max_batch_size);
	wqcfg->mode_support = 1;
}

static void vidxd_mmio_init_engcap(struct vdcm_idxd *vidxd)
{
	u8 *bar0 = vidxd->bar0;
	union engine_cap_reg *engcap = (union engine_cap_reg *)(bar0 + IDXD_ENGCAP_OFFSET);
	struct idxd_wq *wq = vidxd->wq;
	struct idxd_group *group = wq->group;

	engcap->num_engines = group->num_engines;
}

static void vidxd_mmio_init_gencap(struct vdcm_idxd *vidxd)
{
	struct idxd_device *idxd = vidxd->idxd;
	u8 *bar0 = vidxd->bar0;
	union gen_cap_reg *gencap = (union gen_cap_reg *)(bar0 + IDXD_GENCAP_OFFSET);

	gencap->bits = idxd->hw.gen_cap.bits;
	gencap->config_en = 0;
	gencap->max_ims_mult = 0;
	gencap->cmd_cap = 1;
	if (device_user_pasid_enabled(idxd))
		gencap->block_on_fault = 1;
}

static void vidxd_mmio_init_cmdcap(struct vdcm_idxd *vidxd)
{
	struct idxd_device *idxd = vidxd->idxd;
	u8 *bar0 = vidxd->bar0;
	u32 *cmdcap = (u32 *)(bar0 + IDXD_CMDCAP_OFFSET);

	if (idxd->hw.cmd_cap)
		*cmdcap = idxd->hw.cmd_cap;
	else
		*cmdcap = 0x1ffe;

	*cmdcap |= BIT(IDXD_CMD_REQUEST_INT_HANDLE) | BIT(IDXD_CMD_RELEASE_INT_HANDLE) |
			BIT(IDXD_CMD_REVOKED_HANDLES_PROCESSED);
}

static void vidxd_mmio_init_opcap(struct vdcm_idxd *vidxd)
{
	struct idxd_device *idxd = vidxd->idxd;
	u64 opcode;
	u8 *bar0 = vidxd->bar0;
	u64 *opcap = (u64 *)(bar0 + IDXD_OPCAP_OFFSET);

	if (idxd->data->type == IDXD_TYPE_DSA) {
		opcode = BIT_ULL(DSA_OPCODE_NOOP) | BIT_ULL(DSA_OPCODE_BATCH) |
			 BIT_ULL(DSA_OPCODE_DRAIN) | BIT_ULL(DSA_OPCODE_MEMMOVE) |
			 BIT_ULL(DSA_OPCODE_MEMFILL) | BIT_ULL(DSA_OPCODE_COMPARE) |
			 BIT_ULL(DSA_OPCODE_COMPVAL) | BIT_ULL(DSA_OPCODE_CR_DELTA) |
			 BIT_ULL(DSA_OPCODE_AP_DELTA) | BIT_ULL(DSA_OPCODE_DUALCAST) |
			 BIT_ULL(DSA_OPCODE_CRCGEN) | BIT_ULL(DSA_OPCODE_COPY_CRC) |
			 BIT_ULL(DSA_OPCODE_DIF_CHECK) | BIT_ULL(DSA_OPCODE_DIF_INS) |
			 BIT_ULL(DSA_OPCODE_DIF_STRP) | BIT_ULL(DSA_OPCODE_DIF_UPDT) |
			 BIT_ULL(DSA_OPCODE_CFLUSH);
		*opcap = opcode;
	} else if (idxd->data->type == IDXD_TYPE_IAX) {
		opcode = BIT_ULL(IAX_OPCODE_NOOP) | BIT_ULL(IAX_OPCODE_DRAIN) |
			 BIT_ULL(IAX_OPCODE_MEMMOVE);
		*opcap = opcode;
		opcap++;
		opcode = OPCAP_BIT(IAX_OPCODE_DECOMPRESS) | OPCAP_BIT(IAX_OPCODE_COMPRESS) |
			 OPCAP_BIT(IAX_OPCODE_CRC64) | OPCAP_BIT(IAX_OPCODE_ZERO_DECOMP_32) |
			 OPCAP_BIT(IAX_OPCODE_ZERO_DECOMP_16) | OPCAP_BIT(IAX_OPCODE_DECOMP_32) |
			 OPCAP_BIT(IAX_OPCODE_DECOMP_16) | OPCAP_BIT(IAX_OPCODE_SCAN) |
			 OPCAP_BIT(IAX_OPCODE_SET_MEMBER) | OPCAP_BIT(IAX_OPCODE_EXTRACT) |
			 OPCAP_BIT(IAX_OPCODE_SELECT) | OPCAP_BIT(IAX_OPCODE_RLE_BURST) |
			 OPCAP_BIT(IAX_OPCDE_FIND_UNIQUE) | OPCAP_BIT(IAX_OPCODE_EXPAND);
		*opcap = opcode;
	}
}

static void vidxd_mmio_init_version(struct vdcm_idxd *vidxd)
{
	struct idxd_device *idxd = vidxd->idxd;
	u32 *version;

	version = (u32 *)vidxd->bar0;
	*version = idxd->hw.version;
}

static void vidxd_mmio_reset(struct vdcm_idxd *vidxd)
{
	u8 *bar0 = vidxd->bar0;

	memset(bar0 + IDXD_GENCFG_OFFSET, 0, 4);
	memset(bar0 + IDXD_GENCTRL_OFFSET, 0, 4);
	memset(bar0 + IDXD_GENSTATS_OFFSET, 0, 4);
	memset(bar0 + IDXD_INTCAUSE_OFFSET, 0, 4);
	memset(bar0 + IDXD_INTCAUSE_OFFSET, 0, 4);
	memset(bar0 + VIDXD_MSIX_PBA_OFFSET, 0, 1);
	memset(bar0 + VIDXD_MSIX_PERM_OFFSET, 0, VIDXD_MSIX_PERM_TBL_SZ);

	vidxd_mmio_init_grpcfg(vidxd);
	vidxd_mmio_init_wqcfg(vidxd);
}

void vidxd_mmio_init(struct vdcm_idxd *vidxd)
{
	u8 *bar0 = vidxd->bar0;
	union offsets_reg *offsets;

	memset(vidxd->bar0, 0, VIDXD_BAR0_SIZE);

	vidxd_mmio_init_version(vidxd);
	vidxd_mmio_init_gencap(vidxd);
	vidxd_mmio_init_wqcap(vidxd);
	vidxd_mmio_init_grpcap(vidxd);
	vidxd_mmio_init_engcap(vidxd);
	vidxd_mmio_init_opcap(vidxd);

	offsets = (union offsets_reg *)(bar0 + IDXD_TABLE_OFFSET);
	offsets->grpcfg = VIDXD_GRPCFG_OFFSET / 0x100;
	offsets->wqcfg = VIDXD_WQCFG_OFFSET / 0x100;
	offsets->msix_perm = VIDXD_MSIX_PERM_OFFSET / 0x100;

	vidxd_mmio_init_cmdcap(vidxd);
	memset(bar0 + VIDXD_MSIX_PERM_OFFSET, 0, VIDXD_MSIX_PERM_TBL_SZ);
	vidxd_mmio_init_grpcfg(vidxd);
	vidxd_mmio_init_wqcfg(vidxd);
}

static void idxd_complete_command(struct vdcm_idxd *vidxd, enum idxd_cmdsts_err val)
{
	u8 *bar0 = vidxd->bar0;
	u32 *cmd = (u32 *)(bar0 + IDXD_CMD_OFFSET);
	u32 *cmdsts = (u32 *)(bar0 + IDXD_CMDSTS_OFFSET);
	u32 *intcause = (u32 *)(bar0 + IDXD_INTCAUSE_OFFSET);
	struct mdev_device *mdev = vidxd->ivdev.mdev;
	struct device *dev = mdev_dev(mdev);

	*cmdsts = val;
	dev_dbg(dev, "%s: cmd: %#x  status: %#x\n", __func__, *cmd, val);

	if (*cmd & IDXD_CMD_INT_MASK) {
		*intcause |= IDXD_INTC_CMD;
		vidxd_send_interrupt(vidxd, 0);
	}
}

static void vidxd_enable(struct vdcm_idxd *vidxd)
{
	u8 *bar0 = vidxd->bar0;
	union gensts_reg *gensts = (union gensts_reg *)(bar0 + IDXD_GENSTATS_OFFSET);
	struct mdev_device *mdev = vidxd->ivdev.mdev;
	struct device *dev = mdev_dev(mdev);
	bool ats = (*(u16 *)&vidxd->cfg[VIDXD_ATS_OFFSET + 6]) & (1U << 15);
	bool prs = (*(u16 *)&vidxd->cfg[VIDXD_PRS_OFFSET + 4]) & 1U;
	bool pasid = (*(u16 *)&vidxd->cfg[VIDXD_PASID_OFFSET + 6]) & 1U;

	dev_dbg(dev, "%s\n", __func__);
	if (gensts->state == IDXD_DEVICE_STATE_ENABLED)
		return idxd_complete_command(vidxd, IDXD_CMDSTS_ERR_DEV_ENABLED);

	/* Check PCI configuration */
	if (!(vidxd->cfg[PCI_COMMAND] & PCI_COMMAND_MASTER))
		return idxd_complete_command(vidxd, IDXD_CMDSTS_ERR_BUSMASTER_EN);

	if (pasid != prs || (pasid && !ats))
		return idxd_complete_command(vidxd, IDXD_CMDSTS_ERR_BUSMASTER_EN);

	gensts->state = IDXD_DEVICE_STATE_ENABLED;

	return idxd_complete_command(vidxd, IDXD_CMDSTS_SUCCESS);
}

static void vidxd_disable(struct vdcm_idxd *vidxd)
{
	struct idxd_wq *wq;
	union wqcfg *wqcfg;
	u8 *bar0 = vidxd->bar0;
	union gensts_reg *gensts = (union gensts_reg *)(bar0 + IDXD_GENSTATS_OFFSET);
	struct mdev_device *mdev = vidxd->ivdev.mdev;
	struct device *dev = mdev_dev(mdev);
	u32 status;

	dev_dbg(dev, "%s\n", __func__);
	if (gensts->state == IDXD_DEVICE_STATE_DISABLED) {
		idxd_complete_command(vidxd, IDXD_CMDSTS_ERR_DIS_DEV_EN);
		return;
	}

	wqcfg = (union wqcfg *)(bar0 + VIDXD_WQCFG_OFFSET);
	wq = vidxd->wq;

	/* If it is a DWQ, need to disable the DWQ as well */
	if (wq_dedicated(wq)) {
		idxd_wq_disable(wq, false, &status);
		if (status) {
			dev_warn(dev, "vidxd disable (wq disable) failed: %#x\n", status);
			idxd_complete_command(vidxd, IDXD_CMDSTS_ERR_DIS_DEV_EN);
			return;
		}
	} else {
		idxd_wq_drain(wq, &status);
		if (status)
			dev_warn(dev, "vidxd disable (wq drain) failed: %#x\n", status);
	}

	wqcfg->wq_state = 0;
	gensts->state = IDXD_DEVICE_STATE_DISABLED;
	idxd_complete_command(vidxd, IDXD_CMDSTS_SUCCESS);
}

static void vidxd_drain_all(struct vdcm_idxd *vidxd)
{
	struct mdev_device *mdev = vidxd->ivdev.mdev;
	struct device *dev = mdev_dev(mdev);
	struct idxd_wq *wq = vidxd->wq;

	dev_dbg(dev, "%s\n", __func__);

	idxd_wq_drain(wq, NULL);
	idxd_complete_command(vidxd, IDXD_CMDSTS_SUCCESS);
}

static void vidxd_wq_drain(struct vdcm_idxd *vidxd, int val)
{
	struct mdev_device *mdev = vidxd->ivdev.mdev;
	struct device *dev = mdev_dev(mdev);
	u8 *bar0 = vidxd->bar0;
	union wqcfg *wqcfg = (union wqcfg *)(bar0 + VIDXD_WQCFG_OFFSET);
	struct idxd_wq *wq = vidxd->wq;
	u32 status;

	dev_dbg(dev, "%s\n", __func__);
	if (wqcfg->wq_state != IDXD_WQ_DEV_ENABLED) {
		idxd_complete_command(vidxd, IDXD_CMDSTS_ERR_WQ_NOT_EN);
		return;
	}

	idxd_wq_drain(wq, &status);
	if (status) {
		dev_dbg(dev, "wq drain failed: %#x\n", status);
		idxd_complete_command(vidxd, status);
		return;
	}

	idxd_complete_command(vidxd, IDXD_CMDSTS_SUCCESS);
}

static void vidxd_abort_all(struct vdcm_idxd *vidxd)
{
	struct mdev_device *mdev = vidxd->ivdev.mdev;
	struct device *dev = mdev_dev(mdev);
	struct idxd_wq *wq = vidxd->wq;

	dev_dbg(dev, "%s\n", __func__);
	if (wq_dedicated(wq))
		idxd_wq_abort(wq, NULL);
	else
		idxd_wq_drain(wq, NULL);
	idxd_complete_command(vidxd, IDXD_CMDSTS_SUCCESS);
}

static void vidxd_wq_abort(struct vdcm_idxd *vidxd, int val)
{
	struct mdev_device *mdev = vidxd->ivdev.mdev;
	struct device *dev = mdev_dev(mdev);
	u8 *bar0 = vidxd->bar0;
	union wqcfg *wqcfg = (union wqcfg *)(bar0 + VIDXD_WQCFG_OFFSET);
	struct idxd_wq *wq = vidxd->wq;
	u32 status = 0;

	dev_dbg(dev, "%s\n", __func__);
	if (wqcfg->wq_state != IDXD_WQ_DEV_ENABLED) {
		idxd_complete_command(vidxd, IDXD_CMDSTS_ERR_WQ_NOT_EN);
		return;
	}

	if (wq_dedicated(wq))
		idxd_wq_abort(wq, &status);
	else
		idxd_wq_drain(wq, &status);
	if (status) {
		dev_dbg(dev, "wq abort failed: %#x\n", status);
		idxd_complete_command(vidxd, status);
		return;
	}

	idxd_complete_command(vidxd, IDXD_CMDSTS_SUCCESS);
}

void vidxd_reset(struct vdcm_idxd *vidxd)
{
	struct mdev_device *mdev = vidxd->ivdev.mdev;
	struct device *dev = mdev_dev(mdev);
	u8 *bar0 = vidxd->bar0;
	union gensts_reg *gensts = (union gensts_reg *)(bar0 + IDXD_GENSTATS_OFFSET);
	struct idxd_wq *wq;

	dev_dbg(dev, "%s\n", __func__);
	gensts->state = IDXD_DEVICE_STATE_DRAIN;
	wq = vidxd->wq;

	if (wq->state == IDXD_WQ_ENABLED) {
		if (wq_dedicated(wq)) {
			idxd_wq_abort(wq, NULL);
			idxd_wq_disable(wq, false, NULL);
		} else {
			idxd_wq_drain(wq, NULL);
		}
	}

	vidxd_mmio_reset(vidxd);
	gensts->state = IDXD_DEVICE_STATE_DISABLED;
	idxd_complete_command(vidxd, IDXD_CMDSTS_SUCCESS);
}

static void vidxd_wq_reset(struct vdcm_idxd *vidxd, int wq_id_mask)
{
	struct idxd_wq *wq;
	u8 *bar0 = vidxd->bar0;
	union wqcfg *wqcfg = (union wqcfg *)(bar0 + VIDXD_WQCFG_OFFSET);
	struct mdev_device *mdev = vidxd->ivdev.mdev;
	struct device *dev = mdev_dev(mdev);
	u32 status = 0;

	wq = vidxd->wq;
	dev_dbg(dev, "vidxd reset wq %u:%u\n", 0, wq->id);

	if (wqcfg->wq_state != IDXD_WQ_DEV_ENABLED) {
		idxd_complete_command(vidxd, IDXD_CMDSTS_ERR_WQ_NOT_EN);
		return;
	}

	if (wq_dedicated(wq)) {
		idxd_wq_abort(wq, &status);
		if (status) {
			dev_dbg(dev, "vidxd reset wq failed to abort: %#x\n", status);
			idxd_complete_command(vidxd, status);
			return;
		}

		idxd_wq_disable(wq, false, &status);
		if (status) {
			dev_dbg(dev, "vidxd reset wq failed to disable: %#x\n", status);
			idxd_complete_command(vidxd, status);
			return;
		}
	} else {
		idxd_wq_drain(wq, &status);
		if (status) {
			dev_dbg(dev, "vidxd reset wq failed to drain: %#x\n", status);
			idxd_complete_command(vidxd, status);
			return;
		}
	}

	wqcfg->wq_state = IDXD_WQ_DEV_DISABLED;
	idxd_complete_command(vidxd, IDXD_CMDSTS_SUCCESS);
}

static void vidxd_alloc_int_handle(struct vdcm_idxd *vidxd, int operand)
{
	bool ims = !!(operand & CMD_INT_HANDLE_IMS);
	u32 cmdsts;
	struct mdev_device *mdev = vidxd->ivdev.mdev;
	struct device *dev = mdev_dev(mdev);
	int ims_idx, vidx;

	vidx = operand & GENMASK(15, 0);

	dev_dbg(dev, "allocating int handle for %d\n", vidx);

	/* vidx cannot be 0 since that's emulated and does not require IMS handle */
	if (vidx <= 0 || vidx >= VIDXD_MAX_MSIX_VECS) {
		idxd_complete_command(vidxd, IDXD_CMDSTS_ERR_INVAL_INT_IDX);
		return;
	}

	if (ims) {
		dev_warn(dev, "IMS allocation is not implemented yet\n");
		idxd_complete_command(vidxd, IDXD_CMDSTS_ERR_NO_HANDLE);
		return;
	}

	ims_idx = dev_msi_hwirq(dev, vidx - 1);
	cmdsts = ims_idx << IDXD_CMDSTS_RES_SHIFT;
	dev_dbg(dev, "requested index %d handle %d\n", vidx, ims_idx);
	idxd_complete_command(vidxd, cmdsts);
}

static void vidxd_revoked_handles_processed (struct vdcm_idxd *vidxd,
		int operand)
{
	struct mdev_device *mdev = vidxd->ivdev.mdev;
	struct device *dev = mdev_dev(mdev);
	struct idxd_virtual_wq *vwq = &vidxd->vwq;
	int idx;
	u32 status;

        printk("completed revoked int handle\n");

	idxd_complete_command(vidxd, 0);

	BUG_ON(!list_empty(&vwq->head));

	/* Step 1. Drain all the WQs associated with this VM. Currently only 1 */
	idxd_wq_drain(vidxd->wq, &status);

	if (status)
		dev_dbg(dev, "wq drain failed: %#x\n", status);

	/* Step 2. Generate a completion interrupt for all int handles */
	for (idx = 1; idx < VIDXD_MAX_MSIX_VECS; idx++) {
		dev_dbg(dev, "revoked int handle processed idx %d\n", idx);
		vidxd_send_interrupt(vidxd, idx);
	}
}

static void vidxd_release_int_handle(struct vdcm_idxd *vidxd, int operand)
{
	struct mdev_device *mdev = vidxd->ivdev.mdev;
	struct device *dev = mdev_dev(mdev);
	bool ims = !!(operand & CMD_INT_HANDLE_IMS);
	int handle, i;
	bool found = false;

	handle = operand & GENMASK(15, 0);
	dev_dbg(dev, "allocating int handle %d\n", handle);

	if (ims) {
		dev_warn(dev, "IMS allocation is not implemented yet\n");
		idxd_complete_command(vidxd, IDXD_CMDSTS_ERR_INVAL_INT_IDX_RELEASE);
		return;
	}

	/* IMS backed entry start at 1, 0 is emulated vector */
	for (i = 0; i < VIDXD_MAX_MSIX_VECS - 1; i++) {
		if (dev_msi_hwirq(dev, i) == handle) {
			found = true;
			break;
		}
	}

	if (!found) {
		dev_warn(dev, "Freeing unallocated int handle.\n");
		idxd_complete_command(vidxd, IDXD_CMDSTS_ERR_INVAL_INT_IDX_RELEASE);
	}

	dev_dbg(dev, "int handle %d released.\n", handle);
	idxd_complete_command(vidxd, IDXD_CMDSTS_SUCCESS);
}

static void vidxd_wq_enable(struct vdcm_idxd *vidxd, int wq_id)
{
	struct idxd_wq *wq;
	u8 *bar0 = vidxd->bar0;
	union wq_cap_reg *wqcap;
	struct mdev_device *mdev = vidxd->ivdev.mdev;
	struct device *dev = mdev_dev(mdev);
	struct idxd_device *idxd;
	union wqcfg *vwqcfg, *wqcfg;
	int rc;
	bool wq_pasid_enable;
	bool pasid_enabled = (*(u16 *)&vidxd->cfg[VIDXD_PASID_OFFSET + 6]) & 1U;

	if (wq_id >= VIDXD_MAX_WQS) {
		idxd_complete_command(vidxd, IDXD_CMDSTS_INVAL_WQIDX);
		return;
	}

	idxd = vidxd->idxd;
	wq = vidxd->wq;

	dev_dbg(dev, "%s: wq %u:%u\n", __func__, wq_id, wq->id);

	vwqcfg = (union wqcfg *)(bar0 + VIDXD_WQCFG_OFFSET + wq_id * 32);
	wqcap = (union wq_cap_reg *)(bar0 + IDXD_WQCAP_OFFSET);
	wqcfg = wq->wqcfg;

	if (vidxd_state(vidxd) != IDXD_DEVICE_STATE_ENABLED) {
		idxd_complete_command(vidxd, IDXD_CMDSTS_ERR_DEV_NOTEN);
		return;
	}

	if (vwqcfg->wq_state != IDXD_WQ_DEV_DISABLED) {
		idxd_complete_command(vidxd, IDXD_CMDSTS_ERR_WQ_ENABLED);
		return;
	}

	if ((!wq_dedicated(wq) && wqcap->shared_mode == 0) ||
	    (wq_dedicated(wq) && wqcap->dedicated_mode == 0)) {
		idxd_complete_command(vidxd, IDXD_CMDSTS_ERR_WQ_MODE);
		return;
	}

	if ((!wq_dedicated(wq) && vwqcfg->pasid_en == 0) ||
	    (vwqcfg->pasid_en && pasid_enabled == 0)) {
		idxd_complete_command(vidxd, IDXD_CMDSTS_ERR_PASID_EN);
		return;
	}

	wq_pasid_enable = vwqcfg->pasid_en;

	if (wq_dedicated(wq)) {
		int wq_pasid = -1;
		bool priv;

		if (wq_pasid_enable) {
			u32 gpasid;

			priv = vwqcfg->priv;
			gpasid = vwqcfg->pasid;

			if (gpasid == 0) {
				rc = idxd_mdev_get_pasid(mdev, &wq_pasid);
				dev_dbg(dev, "shared wq, pasid 0, use default host: %u\n",
					wq_pasid);
			} else {
				rc = idxd_mdev_get_host_pasid(mdev, gpasid, &wq_pasid);
				dev_dbg(dev, "guest pasid enabled, translate gpasid: %d\n", gpasid);
			}
		} else {
			priv = 1;
			rc = idxd_mdev_get_pasid(mdev, &wq_pasid);
			dev_dbg(dev, "guest pasid disabled, using default host pasid: %u\n",
				wq_pasid);
		}
		if (rc < 0) {
			dev_err(dev, "idxd pasid setup failed wq %d: %d\n", wq->id, rc);
			idxd_complete_command(vidxd, IDXD_CMDSTS_ERR_PASID_EN);
			return;
		}

		if (wq_pasid >= 0) {
			u32 status;
			unsigned long flags;

			wqcfg->bits[WQCFG_PASID_IDX] &= ~GENMASK(29, 8);
			wqcfg->priv = priv;
			wqcfg->pasid_en = 1;
			wqcfg->pasid = wq_pasid;
			dev_dbg(dev, "program pasid %d in wq %d\n", wq_pasid, wq->id);
			spin_lock_irqsave(&idxd->dev_lock, flags);
			idxd_wq_setup_pasid(wq, wq_pasid);
			idxd_wq_setup_priv(wq, priv);
			spin_unlock_irqrestore(&idxd->dev_lock, flags);
			rc = idxd_wq_enable(wq, &status);
			if (rc < 0 || status) {
				dev_err(dev, "vidxd enable wq %d failed\n", wq->id);
				idxd_complete_command(vidxd, status);
				return;
			}
		} else {
			dev_err(dev, "idxd pasid setup failed wq %d wq_pasid %d\n",
				wq->id, wq_pasid);
			idxd_complete_command(vidxd, IDXD_CMDSTS_ERR_PASID_EN);
			return;
		}
	}

	vwqcfg->wq_state = IDXD_WQ_DEV_ENABLED;
	idxd_complete_command(vidxd, IDXD_CMDSTS_SUCCESS);
}

static void vidxd_wq_disable(struct vdcm_idxd *vidxd, int wq_id_mask)
{
	struct idxd_wq *wq;
	union wqcfg *wqcfg, *vwqcfg;
	u8 *bar0 = vidxd->bar0;
	struct mdev_device *mdev = vidxd->ivdev.mdev;
	struct device *dev = mdev_dev(mdev);
	u32 status;

	wq = vidxd->wq;

	dev_dbg(dev, "vidxd disable wq %u:%u\n", 0, wq->id);

	wqcfg = wq->wqcfg;
	vwqcfg = (union wqcfg *)(bar0 + VIDXD_WQCFG_OFFSET);
	if (vwqcfg->wq_state != IDXD_WQ_DEV_ENABLED) {
		idxd_complete_command(vidxd, IDXD_CMDSTS_ERR_WQ_NOT_EN);
		return;
	}

	/* If it is a DWQ, need to disable the DWQ as well */
	if (wq_dedicated(wq)) {
		struct ioasid_set *ioasid_set;
		struct mm_struct *mm;

		idxd_wq_disable(wq, false, &status);
		if (status) {
			dev_warn(dev, "vidxd disable wq failed: %#x\n", status);
			idxd_complete_command(vidxd, status);
			return;
		}

		if (vwqcfg->pasid_en) {
			mm = get_task_mm(current);
			if (!mm) {
				dev_dbg(dev, "Can't retrieve task mm\n");
				return;
			}

			ioasid_set = ioasid_find_mm_set(mm);
			if (!ioasid_set) {
				dev_dbg(dev, "Unable to find ioasid_set\n");
				mmput(mm);
				return;
			}
			mmput(mm);
			if (!ioasid_put(ioasid_set, wqcfg->pasid))
				dev_warn(dev, "Unable to put ioasid\n");
		}
	} else {
		idxd_wq_drain(wq, &status);
		if (status) {
			dev_warn(dev, "vidxd disable drain wq failed: %#x\n", status);
			idxd_complete_command(vidxd, status);
			return;
		}
	}

	vwqcfg->wq_state = IDXD_WQ_DEV_DISABLED;
	idxd_complete_command(vidxd, IDXD_CMDSTS_SUCCESS);
}

void vidxd_free_ims_entries(struct vdcm_idxd *vidxd)
{
	struct mdev_device *mdev = vidxd->ivdev.mdev;
	struct device *dev = mdev_dev(mdev);

	msi_domain_free_irqs(dev_get_msi_domain(dev), dev);
}

static bool command_supported(struct vdcm_idxd *vidxd, u32 cmd)
{
	u8 *bar0 = vidxd->bar0;
	u32 *cmd_cap = (u32 *)(bar0 + IDXD_CMDCAP_OFFSET);

	return !!(*cmd_cap & BIT(cmd));
}

static void vidxd_do_command(struct vdcm_idxd *vidxd, u32 val)
{
	union idxd_command_reg *reg = (union idxd_command_reg *)(vidxd->bar0 + IDXD_CMD_OFFSET);
	struct mdev_device *mdev = vidxd->ivdev.mdev;
	struct device *dev = mdev_dev(mdev);

	reg->bits = val;

	dev_dbg(dev, "%s: cmd code: %u reg: %x\n", __func__, reg->cmd, reg->bits);

	if (!command_supported(vidxd, reg->cmd)) {
		idxd_complete_command(vidxd, IDXD_CMDSTS_INVAL_CMD);
		return;
	}

	switch (reg->cmd) {
	case IDXD_CMD_ENABLE_DEVICE:
		vidxd_enable(vidxd);
		break;
	case IDXD_CMD_DISABLE_DEVICE:
		vidxd_disable(vidxd);
		break;
	case IDXD_CMD_DRAIN_ALL:
		vidxd_drain_all(vidxd);
		break;
	case IDXD_CMD_ABORT_ALL:
		vidxd_abort_all(vidxd);
		break;
	case IDXD_CMD_RESET_DEVICE:
		vidxd_reset(vidxd);
		break;
	case IDXD_CMD_ENABLE_WQ:
		vidxd_wq_enable(vidxd, reg->operand);
		break;
	case IDXD_CMD_DISABLE_WQ:
		vidxd_wq_disable(vidxd, reg->operand);
		break;
	case IDXD_CMD_DRAIN_WQ:
		vidxd_wq_drain(vidxd, reg->operand);
		break;
	case IDXD_CMD_ABORT_WQ:
		vidxd_wq_abort(vidxd, reg->operand);
		break;
	case IDXD_CMD_RESET_WQ:
		vidxd_wq_reset(vidxd, reg->operand);
		break;
	case IDXD_CMD_REQUEST_INT_HANDLE:
		vidxd_alloc_int_handle(vidxd, reg->operand);
		break;
	case IDXD_CMD_RELEASE_INT_HANDLE:
		vidxd_release_int_handle(vidxd, reg->operand);
		break;
	case IDXD_CMD_REVOKED_HANDLES_PROCESSED:
		vidxd_revoked_handles_processed(vidxd, reg->operand);
		break;
	default:
		idxd_complete_command(vidxd, IDXD_CMDSTS_INVAL_CMD);
		break;
	}
}

static void vidxd_send_errors(struct vdcm_idxd *vidxd)
{
	struct idxd_device *idxd = vidxd->idxd;
	u8 *bar0 = vidxd->bar0;
	union sw_err_reg *swerr = (union sw_err_reg *)(bar0 + IDXD_SWERR_OFFSET);
	union genctrl_reg *genctrl = (union genctrl_reg *)(bar0 + IDXD_GENCTRL_OFFSET);
	u32 *intcause = (u32 *)(bar0 + IDXD_INTCAUSE_OFFSET);
	int i;

	lockdep_assert_held(&idxd->dev_lock);

	if (swerr->valid) {
		if (!swerr->overflow)
			swerr->overflow = 1;
		return;
	}

	for (i = 0; i < 4; i++)
		swerr->bits[i] = idxd->sw_err.bits[i];

	*intcause |= IDXD_INTC_ERR;
	if (genctrl->softerr_int_en)
		vidxd_send_interrupt(vidxd, 0);
}

void idxd_wq_vidxd_send_errors(struct idxd_wq *wq)
{
	struct vdcm_idxd *vidxd;

	list_for_each_entry(vidxd, &wq->vdcm_list, list)
		vidxd_send_errors(vidxd);
}
