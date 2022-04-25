// SPDX-License-Identifier: GPL-2.0
/* Copyright(c) 2019,2020 Intel Corporation. All rights rsvd. */
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/pci.h>
#include <linux/device.h>
#include <linux/sched/task.h>
#include <linux/sched/mm.h>
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
#include <linux/circ_buf.h>
#include <linux/irqchip/irq-ims-msi.h>
#include <uapi/linux/idxd.h>
#include "registers.h"
#include "idxd.h"
#include "../mdev_private.h"
#include "mdev.h"

static u64 idxd_pci_config[] = {
	0x0010000000008086ULL,
	0x0080000008800000ULL,
	0x000000000000000cULL,
	0x000000000000000cULL,
	0x0000000000000000ULL,
	0x2010808600000000ULL,
	0x0000004000000000ULL,
	0x000000ff00000000ULL,
	0x0000060000015011ULL, /* MSI-X capability, hardcoded 2 entries, Encoded as N-1 */
	0x0000070000000000ULL,
	0x0000000000920010ULL, /* PCIe capability */
	0x0000000000000000ULL,
	0x0000000000000000ULL,
	0x0000000000000000ULL,
	0x0070001000000000ULL,
	0x0000000000000000ULL,
	0x0000000000000000ULL,
	0x0000000000000000ULL,
};

static u64 idxd_pci_ext_cap[] = {
	0x000000611101000fULL, /* ATS capability */
	0x0000000000000000ULL,
	0x8100000012010013ULL, /* Page Request capability */
	0x0000000000000001ULL,
	0x000014040001001bULL, /* PASID capability */
	0x0000000000000000ULL,
	0x0181808600010023ULL, /* Scalable IOV capability */
	0x0000000100000005ULL,
	0x0000000000000001ULL,
	0x0000000000000000ULL,
};

static int idxd_vdcm_set_irqs(struct vdcm_idxd *vidxd, uint32_t flags,
			      unsigned int index, unsigned int start,
			      unsigned int count, void *data);
static int vidxd_register_ioasid_notifier(struct vdcm_idxd *vidxd);

struct idxd_ioasid_work {
	struct work_struct work;
	struct idxd_wq *wq;
	u32 guest_pasid;
	u32 host_pasid;
};

static const char idxd_dsa_1dwq_name[] = "dsa-1dwq-v1";
static const char idxd_iax_1dwq_name[] = "iax-1dwq-v1";
static const char idxd_dsa_1swq_name[] = "dsa-1swq-v1";
static const char idxd_iax_1swq_name[] = "iax-1swq-v1";

static int idxd_vdcm_get_irq_count(struct mdev_device *mdev, int type)
{
	struct vdcm_idxd *vidxd = mdev_get_drvdata(mdev);
	struct vfio_pci_core_device *vfio_pdev = &vidxd->vfio_pdev;

	/*
	 * Even though the number of MSIX vectors supported are not tied to number of
	 * wqs being exported, the current design is to allow 1 vector per WQ for guest.
	 * So here we end up with num of wqs plus 1 that handles the misc interrupts.
	 */
	if (type == VFIO_PCI_MSI_IRQ_INDEX || type == VFIO_PCI_MSIX_IRQ_INDEX)
		return VIDXD_MAX_MSIX_VECS;
	else if (type == VFIO_PCI_REQ_IRQ_INDEX)
		return 1;
	else if (type >= VFIO_PCI_NUM_IRQS &&
		 type < VFIO_PCI_NUM_IRQS + vfio_pdev->num_ext_irqs)
		return 1;

	return 0;
}

static void idxd_wq_ioasid_work(struct work_struct *work)
{
	struct idxd_ioasid_work *iwork = container_of(work, struct idxd_ioasid_work, work);
	struct idxd_wq *wq = iwork->wq;

	if (wq->state != IDXD_WQ_ENABLED)
		return;

	idxd_device_drain_pasid(wq->idxd, iwork->guest_pasid);
	ioasid_put(NULL, iwork->host_pasid);
	kfree(iwork);
}

static int idxd_mdev_ioasid_event(struct notifier_block *nb, unsigned long event, void *data)
{
	struct idxd_vdev *vdev = container_of(nb, struct idxd_vdev, pasid_nb);
	struct mdev_device *mdev = vdev->mdev;
	struct vdcm_idxd *vidxd = mdev_get_drvdata(mdev);
	struct idxd_wq *wq = vidxd->wq;
	struct ioasid_nb_args *args = (struct ioasid_nb_args *)data;
	struct idxd_ioasid_work *iwork;

	if (event == IOASID_NOTIFY_FREE) {
		dev_dbg(mdev_dev(mdev), "ioasid free event\n");

		if (wq_dedicated(wq))
			return NOTIFY_DONE;

		if (wq->state != IDXD_WQ_ENABLED)
			return NOTIFY_DONE;

		iwork = kmalloc(sizeof(*iwork), GFP_ATOMIC);
		if (!iwork)
			return notifier_from_errno(-ENOMEM);
		iwork->wq = wq;
		iwork->guest_pasid = args->spid;
		iwork->host_pasid = args->id;
		INIT_WORK(&iwork->work, idxd_wq_ioasid_work);
		ioasid_queue_work(&iwork->work);
		return NOTIFY_OK;
	}

	return NOTIFY_OK;
}

int idxd_mdev_get_pasid(struct mdev_device *mdev, u32 *pasid)
{
	struct vfio_group *vfio_group;
	struct iommu_domain *iommu_domain;
	struct device *dev = mdev_dev(mdev);
	struct device *iommu_device = mdev_get_iommu_device(mdev);
	struct vdcm_idxd *vidxd = mdev_get_drvdata(mdev);
	int mdev_pasid;

	if (!vidxd->ivdev.vfio_group) {
		dev_warn(dev, "Missing vfio_group.\n");
		return -EINVAL;
	}

	vfio_group = vidxd->ivdev.vfio_group;

	iommu_domain = vfio_group_iommu_domain(vfio_group);
	if (IS_ERR_OR_NULL(iommu_domain))
		goto err;

	mdev_pasid = iommu_aux_get_pasid(iommu_domain, iommu_device);
	if (mdev_pasid < 0)
		goto err;

	*pasid = (u32)mdev_pasid;
	return 0;

 err:
	vfio_group_put_external_user(vfio_group);
	vidxd->ivdev.vfio_group = NULL;
	return -EFAULT;
}

int idxd_mdev_get_host_pasid(struct mdev_device *mdev, u32 gpasid, u32 *pasid)
{
	struct ioasid_set *ioasid_set;
	struct mm_struct *mm;

	mm = get_task_mm(current);
	if (!mm) {
		dev_warn(mdev_dev(mdev), "%s no mm!\n", __func__);
		return -ENXIO;
	}

	ioasid_set = ioasid_find_mm_set(mm);
	if (!ioasid_set) {
		mmput(mm);
		dev_warn(mdev_dev(mdev), "%s no ioasid_set!\n", __func__);
		return -ENXIO;
	}

	*pasid = ioasid_find_by_spid(ioasid_set, gpasid, true);
	mmput(mm);
	if (*pasid == INVALID_IOASID) {
		dev_warn(mdev_dev(mdev), "%s invalid ioasid by spid!\n", __func__);
		return -ENXIO;
	}

	return 0;
}

static inline void reset_vconfig(struct vdcm_idxd *vidxd)
{
	u16 *devid = (u16 *)(vidxd->cfg + PCI_DEVICE_ID);
	struct idxd_device *idxd = vidxd->idxd;

	memset(vidxd->cfg, 0, VIDXD_MAX_CFG_SPACE_SZ);
	memcpy(vidxd->cfg, idxd_pci_config, sizeof(idxd_pci_config));

	if (idxd->data->type == IDXD_TYPE_DSA)
		*devid = PCI_DEVICE_ID_INTEL_DSA_SPR0;
	else if (idxd->data->type == IDXD_TYPE_IAX)
		*devid = PCI_DEVICE_ID_INTEL_IAX_SPR0;

	memcpy(vidxd->cfg + 0x100, idxd_pci_ext_cap, sizeof(idxd_pci_ext_cap));
}

static inline void reset_vmmio(struct vdcm_idxd *vidxd)
{
	memset(&vidxd->bar0, 0, VIDXD_MAX_MMIO_SPACE_SZ);
}

static inline void vidxd_vwq_init(struct vdcm_idxd *vidxd)
{
	INIT_LIST_HEAD(&vidxd->vwq.head);
	vidxd->vwq.ndescs = 0;

	memset(vidxd->vwq.portals, 0,
		VIDXD_MAX_PORTALS * sizeof(struct idxd_wq_portal));
}

static void idxd_vdcm_init(struct vdcm_idxd *vidxd)
{
	struct idxd_wq *wq = vidxd->wq;

	reset_vconfig(vidxd);
	reset_vmmio(vidxd);

	vidxd->bar_size[0] = VIDXD_BAR0_SIZE;
	vidxd->bar_size[1] = VIDXD_BAR2_SIZE;

	vidxd_mmio_init(vidxd);

	vidxd_vwq_init(vidxd);
	if (wq_dedicated(wq) && wq->state == IDXD_WQ_ENABLED) {
		idxd_wq_disable(wq, false, NULL);
		wq->state = IDXD_WQ_LOCKED;
	}
}

static void  vidxd_unregister_ioasid_notifier(struct vdcm_idxd *vidxd)
{
	struct idxd_vdev *vdev = &vidxd->ivdev;
	struct ioasid_mm_entry *mm_entry, *n;
	struct mm_struct *mm;

	mm = get_task_mm(current);
	if (!mm)
		return;

	mutex_lock(&vdev->ioasid_lock);

	list_for_each_entry_safe(mm_entry, n, &vdev->mm_list, node) {
		if (mm_entry->mm == mm) {
			list_del(&mm_entry->node);
			kfree(mm_entry);
			ioasid_unregister_notifier_mm(mm, &vidxd->ivdev.pasid_nb);
			break;
		}
	}

	mutex_unlock(&vdev->ioasid_lock);
	mmput(mm);
}

static int vidxd_source_pause_device(struct vdcm_idxd *vidxd)
{
	int i;
	int rc;
	u32 status;

	if (vidxd->paused)
		return 0;

	mutex_lock(&vidxd->mig_submit_lock);
	/* The VMM is expected to have unmap the portals. So once we drain
	 * there shouldn't be any work directly submited from the VM */
	vidxd->paused = true;
	mutex_unlock(&vidxd->mig_submit_lock);

	/* For DWQs, pausing the vDSA can always be done by Drain WQ command.
	 * For SWQs, pausing the vDSA may mean Drain PASID if the SWQ is shared
	 * with other VMs. We will need to do Drain PASID for each PASID
	 * allocated to the VM which may take a long time. As an optimization,
	 * we may do Drain PASID if no of PASIDs for the VM is below certain
	 * number and do Drain WQ otherwise.
	 */
	/* Drain WQ(s) to make sure no more outstanding work in the dev */
	/* TODO: Currently support for only 1 WQ per VDev */
	for (i = 0; i < vidxd->num_wqs; i++) {
		rc = idxd_wq_drain(vidxd->wq, &status);

		if (rc < 0) {
			pr_info("%s: failed rc %d\n", __func__, rc);
			return rc;
		}
	}
	return 0;
}

static void vidxd_free_resources (struct vdcm_idxd *vidxd)
{
	int i;

        /* Free the queued descriptors */
        for (i = 0; i < vidxd->num_wqs; i++) {
                struct idxd_wq_desc_elem *el, *tmp;
		struct idxd_virtual_wq *vwq = &vidxd->vwq;

                list_for_each_entry_safe(el, tmp, &vwq->head, link) {
                        list_del(&el->link);
                        vwq->ndescs--;
                        kfree(el);
                }
        }

}

static void vidxd_source_prepare_for_migration(struct vdcm_idxd *vidxd)
{
	int i;
	struct vfio_pci_core_device *vdev = &vidxd->vfio_pdev;
	struct vfio_device_migration_info *mig_info =
		(struct vfio_device_migration_info *)vdev->mig_pages;
	u8 *data_ptr = (u8 *)vdev->mig_pages;
	unsigned int offset =  mig_info->data_offset;
	struct mdev_device *mdev = vidxd->ivdev.mdev;
	struct device *dev = mdev_dev(mdev);
	struct idxd_virtual_wq *vwq;

	memcpy(data_ptr + offset, vidxd->cfg, sizeof(vidxd->cfg));
	offset += sizeof(vidxd->cfg);
	memcpy(data_ptr + offset, (u8 *)vidxd->bar_val, sizeof(vidxd->bar_val));
	offset += sizeof(vidxd->bar_val);
	memcpy(data_ptr + offset, (u8 *)vidxd->bar_size,
					sizeof(vidxd->bar_size));
	offset += sizeof(vidxd->bar_size);
	memcpy(data_ptr + offset, (u8 *)&vidxd->bar0, sizeof(vidxd->bar0));
	offset += sizeof(vidxd->bar0);

	/* Save int handle info */
	for (i = 1; i < VIDXD_MAX_MSIX_VECS; i++) {
		u32 ims_idx = dev_msi_hwirq(dev, i - 1);

		/* Save the current handle in use */
		pr_info("Saving handle %d at offset %x\n", ims_idx, offset);
		memcpy(data_ptr + offset, (u8 *)&ims_idx, sizeof(ims_idx));
		offset += sizeof(ims_idx);
	}

        /* Save the queued descriptors */
        for (i = 0; i < vidxd->num_wqs; i++) {
                struct idxd_wq_desc_elem *el;
                vwq = &vidxd->vwq;

                memcpy(data_ptr + offset, (u8 *)&vwq->ndescs, sizeof(vwq->ndescs));
                offset += sizeof(vwq->ndescs);
                list_for_each_entry(el, &vwq->head, link) {
                        printk("Saving descriptor at offset %x\n", offset);
                        memcpy(data_ptr + offset, (u8 *)el, sizeof(*el));
                        offset += sizeof(*el);
                }
        }

	mig_info->data_size = offset - mig_info->data_offset;
	mig_info->pending_bytes = offset - mig_info->data_offset;

	dev_dbg(dev, "%s, mig_info->pending_bytes: 0x%llx, data_size: 0x%llx\n",
		__func__, mig_info->pending_bytes, mig_info->data_size);
}

static void vidxd_dest_prepare_for_migration(struct vdcm_idxd *vidxd)
{

}

static int vidxd_resume_wq_state(struct vdcm_idxd *vidxd)
{
	struct idxd_wq *wq;
	struct mdev_device *mdev = vidxd->ivdev.mdev;
	struct device *dev = mdev_dev(mdev);
	struct idxd_device *idxd = vidxd->idxd;
	union wqcfg *vwqcfg, *wqcfg;
	bool priv;
	int wq_id;
	int rc = 0;
	u8 *bar0 = vidxd->bar0;

	dev_dbg(dev, "%s:%d numwqs %d\n", __func__, __LINE__, vidxd->num_wqs);
	/* TODO: Currently support for only 1 WQ per VDev */
	for (wq_id = 0; wq_id < vidxd->num_wqs; wq_id++) {
		wq = vidxd->wq;
		dev_dbg(dev, "%s:%d wq %px\n", __func__, __LINE__, wq);
		vwqcfg = (union wqcfg *)(bar0 + VIDXD_WQCFG_OFFSET);
		wqcfg = wq->wqcfg;

		if (vidxd_state(vidxd) != 1 || vwqcfg->wq_state != 1) {
			/* either VDEV or vWQ is disabled */
			if (wq_dedicated(wq) && wq->state == IDXD_WQ_ENABLED)
				idxd_wq_disable(wq, false, NULL);
			continue;
		} else {
			unsigned long flags;
			printk("vidxd re-enable wq %u:%u\n", wq_id, wq->id);

			/* If dedicated WQ and PASID is not enabled, program
			 * the default PASID in the WQ PASID register */
			if (wq_dedicated(wq) && vwqcfg->mode_support) {
				int wq_pasid = -1, gpasid = -1;

				if (vwqcfg->pasid_en) {
					gpasid = vwqcfg->pasid;
					priv = vwqcfg->priv;
					rc = idxd_mdev_get_host_pasid(mdev,
						gpasid, &wq_pasid);
				} else {
					rc = idxd_mdev_get_pasid(mdev,
						&wq_pasid);
					priv = true;
				}

				if (wq_pasid >= 0) {
					wqcfg->bits[WQCFG_PASID_IDX] &=
								~GENMASK(29, 8);
					wqcfg->priv = priv;
					wqcfg->pasid_en = 1;
					wqcfg->pasid = wq_pasid;
					dev_dbg(dev, "pasid %d:%d in wq %d\n",
						gpasid, wq_pasid, wq->id);
					spin_lock_irqsave(&idxd->dev_lock,
									flags);
					idxd_wq_setup_pasid(wq, wq_pasid);
					idxd_wq_setup_priv(wq, priv);
					spin_unlock_irqrestore(&idxd->dev_lock,
									flags);
					rc = idxd_wq_enable(wq, NULL);
					if (rc) {
						dev_err(dev, "resume wq failed\n");
						break;;
					}
				}
			} else if (!wq_dedicated(wq) && vwqcfg->mode_support) {
				wqcfg->bits[WQCFG_PASID_IDX] &= ~GENMASK(29, 8);
				wqcfg->pasid_en = 1;
				wqcfg->mode = 0;
				spin_lock_irqsave(&idxd->dev_lock, flags);
				idxd_wq_setup_pasid(wq, 0);
				spin_unlock_irqrestore(&idxd->dev_lock, flags);
				rc = idxd_wq_enable(wq, NULL);
				if (rc) {
					dev_err(dev, "resume wq %d failed\n",
							wq->id);
					break;
				}
			}
		}
	}
	return rc;
}

static unsigned int vidxd_dest_load_state(struct vdcm_idxd *vidxd)
{
	struct vfio_pci_core_device *vdev = &vidxd->vfio_pdev;
	struct vfio_device_migration_info *mig_info =
		(struct vfio_device_migration_info *)vdev->mig_pages;
	u8	*data_ptr = (u8 *)vdev->mig_pages;
	unsigned int offset =  mig_info->data_offset;

	pr_info("%s, data_size: %llx, data_offset: 0x%llx\n", __func__,
			mig_info->data_size, mig_info->data_offset);

	/* restore the state data to device */
	memcpy(vidxd->cfg, data_ptr + offset, sizeof(vidxd->cfg));
	offset += sizeof(vidxd->cfg);
	memcpy((u8 *)vidxd->bar_val, data_ptr + offset, sizeof(vidxd->bar_val));
	offset += sizeof(vidxd->bar_val);
	memcpy((u8 *)vidxd->bar_size, data_ptr + offset,
					sizeof(vidxd->bar_size));
	offset += sizeof(vidxd->bar_size);
	memcpy((u8 *)&vidxd->bar0, data_ptr + offset, sizeof(vidxd->bar0));
	offset += sizeof(vidxd->bar0);
	//memcpy((u8 *)ims, data_ptr + offset, sizeof(vidxd->ims));
	//offset += sizeof(vidxd->ims);

	printk("Offset %x\n", offset);
	return offset;
}

static int vidxd_resume_ims_state (struct vdcm_idxd *vidxd,
		unsigned int *offset, bool *int_handle_revoked)
{
	struct vfio_pci_core_device *vdev = &vidxd->vfio_pdev;
	u8 *data_ptr = (u8 *)vdev->mig_pages;
	u8 *bar0 = vidxd->bar0;
	int i;
	int rc = 0;
	struct mdev_device *mdev = vidxd->ivdev.mdev;
	struct device *dev = mdev_dev(mdev);

	/* Restore int handle info */
	for (i = 1; i < VIDXD_MAX_MSIX_VECS; i++) {
		u32 perm_val, auxval;
		u32 gpasid, pasid;
		bool paside;
		int ims_idx = dev_msi_hwirq(dev, i - 1);
		int irq = dev_msi_irq_vector(dev, i - 1);
		u32 revoked_handle;

		memcpy((u8 *)&revoked_handle, data_ptr + *offset,
					sizeof(revoked_handle));
		*offset += sizeof(revoked_handle);

		pr_info("%s: %d new handle %x old handle %x\n",
				__func__, i, ims_idx, revoked_handle);

		if (revoked_handle != ims_idx) {
			/* Int Handle Revoked */
			*int_handle_revoked = true;
		}

		perm_val = *(u32 *)(bar0 + VIDXD_MSIX_PERM_OFFSET + i * 8);

		paside = (perm_val >> 3) & 1;
		gpasid = (perm_val >> 12) & 0xfffff;

		if (paside)
			rc = idxd_mdev_get_host_pasid(vidxd->ivdev.mdev, gpasid, &pasid);
		else
			rc = idxd_mdev_get_pasid(vidxd->ivdev.mdev, &pasid);
		if (rc < 0)
			return rc;

		auxval = ims_ctrl_pasid_aux(pasid, true);

		rc = irq_set_auxdata(irq, IMS_AUXDATA_CONTROL_WORD, auxval);
		pr_info("%s: auxval %x rc %d\n", __func__, auxval, rc);
		if (rc < 0) {
			pr_info("set ims pasid failed rc %d\n", rc);
			break;
		}
	}

	return rc;
}

static int vidxd_resubmit_pending_descs (struct vdcm_idxd *vidxd,
		unsigned int *offset)
{
	struct vfio_pci_core_device *vdev = &vidxd->vfio_pdev;
	struct mdev_device *mdev = vidxd->ivdev.mdev;
	u8 *data_ptr = (u8 *)vdev->mig_pages;
	struct idxd_virtual_wq *vwq;
	struct idxd_wq *wq;
	int i;

	/* Submit the queued descriptors. The WQ state
	 * has been resumed by this point
	 */
	for (i = 0; i < vidxd->num_wqs; i++) {
		void __iomem *portal;
		struct idxd_wq_desc_elem el;
		vwq = &vidxd->vwq;
		wq = vidxd->wq;

		memcpy((u8 *)&vwq->ndescs, data_ptr + *offset, sizeof(vwq->ndescs));
		*offset += sizeof(vwq->ndescs);

		for (; vwq->ndescs > 0; vwq->ndescs--) {
			printk("Descriptor at offset %x\n", *offset);

			memcpy((u8 *)&el, data_ptr + *offset, sizeof(el));
			*offset += sizeof(el);

			portal = vidxd->idxd->portal_base +
				idxd_get_wq_portal_full_offset(wq->id,
						el.portal_prot, IDXD_IRQ_IMS);
			portal += (el.portal_id << 6);

			pr_info("submitting a desc to WQ %d:%d ded %d\n",
					i, wq->id, wq_dedicated(wq));
			if (wq_dedicated(wq)) {
				iosubmit_cmds512(portal, el.work_desc, 1);
			} else {
				int rc;
				struct dsa_hw_desc *hw =
					(struct dsa_hw_desc *)el.work_desc;
				int hpasid, gpasid = hw->pasid;

				/* Translate the gpasid in the descriptor */
				rc = idxd_mdev_get_host_pasid(mdev,
						gpasid, &hpasid);
				if (rc < 0) {
					pr_info("gpasid->hpasid trans failed\n");
					continue;
				}
				hw->pasid = hpasid;
				/* FIXME: Allow enqcmds to retry a few times
				 * before failing */
				rc = enqcmds(portal, el.work_desc);
				if (rc < 0) {
					pr_info("%s: enqcmds failed\n", __func__);
					continue;
				}
			}
		}
	}

	return 0;
}

static int vidxd_dest_complete_migration(struct vdcm_idxd *vidxd)
{
	int rc = 0;
	unsigned int offset;
	bool int_handle_revoked = false;

	offset = vidxd_dest_load_state(vidxd);

	rc = vidxd_resume_wq_state(vidxd);

	if (rc) {
		pr_info("vidxd resume wq state failed %d\n", rc);
		return rc;
	}

	rc = vidxd_resume_ims_state(vidxd, &offset, &int_handle_revoked);

	if (rc) {
		pr_info("vidxd int handle revocation handling failed %d\n", rc);
		return rc;
	}

	rc = vidxd_resubmit_pending_descs(vidxd, &offset);

	if (rc) {
		pr_info("vidxd pending descs handling failed %d\n", rc);
		return rc;
	}

	if (int_handle_revoked)
                vidxd_notify_revoked_handles(vidxd);

	return rc;
}

static int vidxd_migration_state_change(struct vfio_pci_core_device *vfio_vdev,
		u32 new_state)
{
	struct vdcm_idxd *vidxd = container_of(vfio_vdev, struct vdcm_idxd, vfio_pdev);
	struct vfio_device_migration_info *mig_info =
		(struct vfio_device_migration_info *) vfio_vdev->mig_pages;
	int ret = 0;

	pr_info("%s, VFIO_DEVICE_STATE_MASK: 0x%x, new_state: 0x%x\n",
			__func__, VFIO_DEVICE_STATE_MASK, new_state);
	if (new_state & (~(VFIO_DEVICE_STATE_MASK))) {
		pr_info("%s, invalid new device state, 0x%x!!\n", __func__, new_state);
		return -EINVAL;
	}

	switch (new_state) {
	case 0:
		pr_info("%s, __STOPPED !!\n", __func__);
		vidxd_free_resources(vidxd);
		break;
	case VFIO_DEVICE_STATE_RUNNING:
		pr_info("%s, VFIO_DEVICE_STATE_RUNNING!! old state %x\n",
			__func__, mig_info->device_state);
		if (mig_info->device_state & VFIO_DEVICE_STATE_RESUMING)
			vidxd_dest_complete_migration(vidxd);

		mutex_lock(&vidxd->mig_submit_lock);
		/* The VMM may continue the VM after pausing it. So get ready
		* for normal operation */
		vidxd->paused = false;
		mutex_unlock(&vidxd->mig_submit_lock);

		break;
	case VFIO_DEVICE_STATE_SAVING | VFIO_DEVICE_STATE_RUNNING:
		pr_info("%s, VFIO_DEVICE_STATE_SAVING | VFIO_DEVICE_STATE_RUNNING!!\n", __func__);

		break;
	case VFIO_DEVICE_STATE_SAVING:
		pr_info("%s, VFIO_DEVICE_STATE_SAVING!!\n", __func__);
		/* Prepared the state data for migration */
		if (!(mig_info->device_state & VFIO_DEVICE_STATE_RUNNING))
			vidxd_source_prepare_for_migration(vidxd);

		/* Pause the virtual device. The vCPUs are still running.
		 * This happens just before the VM is paused. The vDEV
		 * is already in slow path */
		if (mig_info->device_state & VFIO_DEVICE_STATE_RUNNING)
			vidxd_source_pause_device(vidxd);
		break;
	case VFIO_DEVICE_STATE_RESUMING:
		/* Prepared the state restore for migration */
		vidxd_dest_prepare_for_migration(vidxd);
		pr_info("%s, VFIO_DEVICE_STATE_RESUMING!!\n", __func__);
		break;
	default:
		pr_info("%s, not handled new device state: 0x%x\n", __func__, new_state);
		ret = -EINVAL;
	}
	return ret;
}

static struct vfio_pci_migops vidxd_migops = {
	.state_change	= vidxd_migration_state_change,
};

static struct idxd_wq *find_any_dwq(struct idxd_device *idxd, struct vdcm_idxd_type *type)
{
	int i;
	struct idxd_wq *wq;
	unsigned long flags;

	switch (type->type) {
	case IDXD_MDEV_TYPE_DSA_1_DWQ:
		if (idxd->data->type != IDXD_TYPE_DSA)
			return NULL;
		break;
	case IDXD_MDEV_TYPE_IAX_1_DWQ:
		if (idxd->data->type != IDXD_TYPE_IAX)
			return NULL;
		break;
	default:
		return NULL;
	}

	spin_lock_irqsave(&idxd->dev_lock, flags);
	for (i = 0; i < idxd->max_wqs; i++) {
		wq = idxd->wqs[i];

		if (wq->state != IDXD_WQ_ENABLED && wq->state != IDXD_WQ_LOCKED)
			continue;

		if (!is_idxd_wq_mdev(wq))
			continue;

		if (!wq_dedicated(wq))
			continue;

		if (idxd_wq_refcount(wq) != 0)
			continue;

		spin_unlock_irqrestore(&idxd->dev_lock, flags);
		mutex_lock(&wq->wq_lock);
		idxd_wq_get(wq);
		mutex_unlock(&wq->wq_lock);
		return wq;
	}

	spin_unlock_irqrestore(&idxd->dev_lock, flags);
	return NULL;
}

static int swq_lowest_client_count(struct idxd_device *idxd)
{
	struct idxd_wq *wq;
	int i, count = -ENODEV;

	lockdep_assert_held(&idxd->dev_lock);
	for (i = 0; i < idxd->max_wqs; i++) {
		wq = idxd->wqs[i];

		if (wq->state != IDXD_WQ_ENABLED)
			continue;

		if (!is_idxd_wq_mdev(wq))
			continue;

		if (wq_dedicated(wq))
			continue;

		if (count == -ENODEV)
			count = idxd_wq_refcount(wq);
		else if (count > idxd_wq_refcount(wq))
			count = idxd_wq_refcount(wq);
	}

	return count;
}

static struct idxd_wq *find_any_swq(struct idxd_device *idxd, struct vdcm_idxd_type *type)
{
	int i, count;
	struct idxd_wq *wq;
	unsigned long flags;

	switch (type->type) {
	case IDXD_MDEV_TYPE_DSA_1_SWQ:
		if (idxd->data->type != IDXD_TYPE_DSA)
			return NULL;
		break;
	case IDXD_MDEV_TYPE_IAX_1_SWQ:
		if (idxd->data->type != IDXD_TYPE_IAX)
			return NULL;
		break;
	default:
		return NULL;
	}

	spin_lock_irqsave(&idxd->dev_lock, flags);
	count = swq_lowest_client_count(idxd);
	if (count < 0)
		goto out;

	for (i = 0; i < idxd->max_wqs; i++) {
		wq = idxd->wqs[i];

		if (wq->state != IDXD_WQ_ENABLED)
			continue;

		if (!is_idxd_wq_mdev(wq))
			continue;

		if (wq_dedicated(wq))
			continue;

		/*
		 * Attempt to load balance the shared wq by round robin until on the lowest
		 * ref count for the wq.
		 */
		if (idxd_wq_refcount(wq) != count)
			continue;

		spin_unlock_irqrestore(&idxd->dev_lock, flags);
		mutex_lock(&wq->wq_lock);
		idxd_wq_get(wq);
		mutex_unlock(&wq->wq_lock);
		return wq;
	}

 out:
	spin_unlock_irqrestore(&idxd->dev_lock, flags);
	return NULL;
}

extern const struct vfio_pci_regops vfio_pci_dma_fault_regops;

static struct vdcm_idxd *vdcm_vidxd_create(struct idxd_device *idxd, struct mdev_device *mdev,
					   struct vdcm_idxd_type *type)
{
	struct vdcm_idxd *vidxd;
	struct device *dev = mdev_dev(mdev);
	struct idxd_wq *wq = NULL;
	int rc;

	switch (type->type) {
	case IDXD_MDEV_TYPE_DSA_1_DWQ:
	case IDXD_MDEV_TYPE_IAX_1_DWQ:
		wq = find_any_dwq(idxd, type);
		break;
	case IDXD_MDEV_TYPE_DSA_1_SWQ:
	case IDXD_MDEV_TYPE_IAX_1_SWQ:
		wq = find_any_swq(idxd, type);
		break;
	default:
		return ERR_PTR(-ENODEV);
	}

	if (!wq)
		return ERR_PTR(-ENODEV);

	vidxd = kzalloc(sizeof(*vidxd), GFP_KERNEL);
	if (!vidxd) {
		rc = -ENOMEM;
		goto err;
	}

	mutex_init(&vidxd->dev_lock);
	vidxd->idxd = idxd;
	vidxd->ivdev.mdev = mdev;
	vidxd->wq = wq;
	mdev_set_drvdata(mdev, vidxd);
	vidxd->type = type;
	vidxd->num_wqs = VIDXD_MAX_WQS;
	dev_set_msi_domain(dev, idxd->ims_domain);
	mutex_init(&vidxd->ivdev.ioasid_lock);
	INIT_LIST_HEAD(&vidxd->ivdev.mm_list);

	idxd_vdcm_init(vidxd);

	mutex_init(&vidxd->vfio_pdev.igate);
	vidxd->vfio_pdev.pdev = idxd->pdev;
	rc = vfio_pci_dma_fault_init(&vidxd->vfio_pdev, false);
	if (rc < 0) {
		dev_err(dev, "dma fault region init failed\n");
		kfree(vidxd);
		goto err;
	}

	mdev_set_iommu_fault_data(mdev, &vidxd->vfio_pdev);

	mutex_init(&vidxd->mig_submit_lock);
	vidxd->vfio_pdev.migops = &vidxd_migops;
	rc = vfio_pci_migration_init(&vidxd->vfio_pdev, VIDXD_STATE_BUFFER_SIZE);
	if (rc)
		pr_err("%s, idxd migration region init failed!!!\n", __func__);
	else
		pr_info("%s, idxd migration region init successfully!!!\n", __func__);

	return vidxd;

 err:
	mutex_lock(&wq->wq_lock);
	idxd_wq_put(wq);
	mutex_unlock(&wq->wq_lock);
	return ERR_PTR(rc);
}

static struct vdcm_idxd_type idxd_mdev_types[IDXD_MDEV_TYPES] = {
	{
		.name = idxd_dsa_1dwq_name,
		.type = IDXD_MDEV_TYPE_DSA_1_DWQ,
	},
	{
		.name = idxd_iax_1dwq_name,
		.type = IDXD_MDEV_TYPE_IAX_1_DWQ,
	},
	{
		.name = idxd_dsa_1swq_name,
		.type = IDXD_MDEV_TYPE_DSA_1_SWQ,
	},
	{
		.name = idxd_iax_1swq_name,
		.type = IDXD_MDEV_TYPE_IAX_1_SWQ,
	},
};

static struct vdcm_idxd_type *idxd_vdcm_get_type(struct mdev_device *mdev)
{
	return &idxd_mdev_types[mdev_get_type_group_id(mdev)];
}

static const struct vfio_device_ops idxd_mdev_ops;

static int idxd_vdcm_probe(struct mdev_device *mdev)
{
	struct vdcm_idxd *vidxd;
	struct vdcm_idxd_type *type;
	struct device *dev, *parent;
	struct idxd_device *idxd;
	struct idxd_wq *wq;
	int rc;

	parent = mdev_parent_dev(mdev);
	idxd = dev_get_drvdata(parent);
	dev = mdev_dev(mdev);
	mdev_set_iommu_device(mdev, parent);
	type = idxd_vdcm_get_type(mdev);

	vidxd = vdcm_vidxd_create(idxd, mdev, type);
	if (IS_ERR(vidxd)) {
		dev_err(dev, "failed to create vidxd: %ld\n", PTR_ERR(vidxd));
		return PTR_ERR(vidxd);
	}

	vfio_init_group_dev(&vidxd->vdev, &mdev->dev, &idxd_mdev_ops);
	wq = vidxd->wq;
	dev_set_drvdata(dev, vidxd);
	rc = vfio_register_group_dev(&vidxd->vdev);
	if (rc < 0) {
		mutex_lock(&wq->wq_lock);
		idxd_wq_put(wq);
		mutex_unlock(&wq->wq_lock);
		kfree(vidxd);
		return rc;
	}

	mutex_lock(&wq->wq_lock);
	list_add(&vidxd->list, &wq->vdcm_list);
	mutex_unlock(&wq->wq_lock);
	dev_dbg(dev, "mdev creation success: %s\n", dev_name(mdev_dev(mdev)));

	return 0;
}

static void idxd_vdcm_remove(struct mdev_device *mdev)
{
	struct vdcm_idxd *vidxd = mdev_get_drvdata(mdev);
	struct idxd_device *idxd = vidxd->idxd;
	struct device *dev = &idxd->pdev->dev;
	struct idxd_wq *wq = vidxd->wq;
	struct vfio_pci_core_device *vfio_pdev = &vidxd->vfio_pdev;
	int i;

	dev_dbg(dev, "%s: removing for wq %d\n", __func__, vidxd->wq->id);

	for (i = 0; i < vfio_pdev->num_regions; i++)
		vfio_pdev->region[i].ops->release(vfio_pdev, &vfio_pdev->region[i]);
	vfio_pdev->num_regions = 0;
	kfree(vfio_pdev->region);
	vfio_pdev->region = NULL;

	for (i = 0; i < vfio_pdev->num_ext_irqs; i++)
		vfio_pci_set_ext_irq_trigger(vfio_pdev,
					 VFIO_IRQ_SET_DATA_NONE | VFIO_IRQ_SET_ACTION_TRIGGER,
					 VFIO_PCI_NUM_IRQS + i, 0, 0, NULL);
	vfio_pdev->num_ext_irqs = 0;
	kfree(vfio_pdev->ext_irqs);
	vfio_pdev->ext_irqs = NULL;

	mutex_lock(&wq->wq_lock);
	list_del(&vidxd->list);
	idxd_wq_put(wq);
	mutex_unlock(&wq->wq_lock);

	vfio_unregister_group_dev(&vidxd->vdev);

	kfree(vidxd);
}

static int idxd_vdcm_open(struct vfio_device *vdev)
{
	struct vdcm_idxd *vidxd = vdev_to_vidxd(vdev);
	int rc = -EINVAL;
	struct vdcm_idxd_type *type = vidxd->type;
	struct device *dev = vdev->dev;
	struct vfio_group *vfio_group;

	dev_dbg(dev, "%s: type: %d\n", __func__, type->type);

	vfio_group = vfio_group_get_external_user_from_dev(dev);
	if (IS_ERR_OR_NULL(vfio_group)) {
		rc = -EFAULT;
		goto out;
	}

	rc = vidxd_register_ioasid_notifier(vidxd);
	if (rc < 0)
		goto ioasid_err;

	mutex_lock(&vidxd->dev_lock);
	if (vidxd->refcount)
		goto ioasid_err;

	vidxd->ivdev.vfio_group = vfio_group;
	vidxd->refcount++;

	mutex_unlock(&vidxd->dev_lock);
	return 0;

 ioasid_err:
	vfio_group_put_external_user(vfio_group);
 out:
	mutex_unlock(&vidxd->dev_lock);
	return rc;
}

static void idxd_vdcm_close(struct vfio_device *vdev)
{
	struct vdcm_idxd *vidxd = vdev_to_vidxd(vdev);

	mutex_lock(&vidxd->dev_lock);
	if (!vidxd->refcount)
		goto out;

	vidxd_unregister_ioasid_notifier(vidxd);
	idxd_vdcm_set_irqs(vidxd, VFIO_IRQ_SET_DATA_NONE | VFIO_IRQ_SET_ACTION_TRIGGER,
			   VFIO_PCI_MSIX_IRQ_INDEX, 0, 0, NULL);

	if (vidxd->ivdev.vfio_group) {
		vfio_group_put_external_user(vidxd->ivdev.vfio_group);
		vidxd->ivdev.vfio_group = NULL;
	}

	/* Re-initialize the VIDXD to a pristine state for re-use */
	idxd_vdcm_init(vidxd);
	vidxd->refcount--;
	vidxd->paused = false;
 out:
	mutex_unlock(&vidxd->dev_lock);
}

static int vidxd_register_ioasid_notifier(struct vdcm_idxd *vidxd)
{
	struct idxd_vdev *vdev = &vidxd->ivdev;
	struct ioasid_mm_entry *mm_entry;
	struct mm_struct *mm;
	int rc;

	mm = get_task_mm(current);
	if (!mm)
		return -ENODEV;

	mutex_lock(&vdev->ioasid_lock);
	list_for_each_entry(mm_entry, &vdev->mm_list, node) {
		if (mm_entry->mm == mm) {
			mutex_unlock(&vdev->ioasid_lock);
			mmput(mm);
			return 0;
		}
	}

	mm_entry = kzalloc(sizeof(*mm_entry), GFP_KERNEL);
	if (!mm_entry) {
		rc = -ENOMEM;
		goto err_alloc;
	}

	mm_entry->mm = mm;

	vidxd->ivdev.pasid_nb.priority = IOASID_PRIO_DEVICE;
	vidxd->ivdev.pasid_nb.notifier_call = idxd_mdev_ioasid_event;
	rc = ioasid_register_notifier_mm(mm, &vidxd->ivdev.pasid_nb);
	if (rc < 0)
		goto err_ioasid;

	list_add(&mm_entry->node, &vdev->mm_list);
	mutex_unlock(&vdev->ioasid_lock);
	mmput(mm);

	return 0;

 err_ioasid:
	kfree(mm_entry);
 err_alloc:
	mutex_unlock(&vdev->ioasid_lock);
	mmput(mm);
	return rc;
}

static ssize_t idxd_vdcm_rw(struct vfio_device *vdev, char *buf, size_t count, loff_t *ppos,
			    enum idxd_vdcm_rw mode)
{
	struct vdcm_idxd *vidxd = vdev_to_vidxd(vdev);
	unsigned int index = VFIO_PCI_OFFSET_TO_INDEX(*ppos);
	u64 pos = *ppos & VFIO_PCI_OFFSET_MASK;
	struct device *dev = vdev->dev;
	struct vfio_pci_core_device *vfio_pdev = &vidxd->vfio_pdev;
	int rc = -EINVAL;

	if (index >= VFIO_PCI_NUM_REGIONS + vfio_pdev->num_regions) {
		dev_err(dev, "invalid index: %u\n", index);
		return -EINVAL;
	}

	switch (index) {
	case VFIO_PCI_CONFIG_REGION_INDEX:
		if (mode == IDXD_VDCM_WRITE)
			rc = vidxd_cfg_write(vidxd, pos, buf, count);
		else
			rc = vidxd_cfg_read(vidxd, pos, buf, count);
		break;
	case VFIO_PCI_BAR0_REGION_INDEX:
	case VFIO_PCI_BAR1_REGION_INDEX:
		if (mode == IDXD_VDCM_WRITE)
			rc = vidxd_mmio_write(vidxd, vidxd->bar_val[0] + pos, buf, count);
		else
			rc = vidxd_mmio_read(vidxd, vidxd->bar_val[0] + pos, buf, count);
		break;
	case VFIO_PCI_BAR2_REGION_INDEX:
	case VFIO_PCI_BAR3_REGION_INDEX:
		if (mode == IDXD_VDCM_WRITE) {
			rc = vidxd_portal_mmio_write(vidxd,
				vidxd->bar_val[1] + pos, buf, count);
		} else {
			rc = vidxd_portal_mmio_read(vidxd,
				vidxd->bar_val[1] + pos, buf, count);
		}
		break;

	case VFIO_PCI_BAR4_REGION_INDEX:
	case VFIO_PCI_BAR5_REGION_INDEX:
	case VFIO_PCI_VGA_REGION_INDEX:
	case VFIO_PCI_ROM_REGION_INDEX:
		dev_err(dev, "unsupported region: %u\n", index);
		break;

	default:
		dev_dbg(dev, "vendor specific region: %u\n", index);
		index -= VFIO_PCI_NUM_REGIONS;
		return vfio_pdev->region[index].ops->rw(vfio_pdev, buf, count, ppos, mode);
	}

	return rc == 0 ? count : rc;
}

static ssize_t idxd_vdcm_read(struct vfio_device *vdev, char __user *buf, size_t count,
			      loff_t *ppos)
{
	struct vdcm_idxd *vidxd = vdev_to_vidxd(vdev);
	unsigned int index = VFIO_PCI_OFFSET_TO_INDEX(*ppos);
	unsigned int done = 0;
	int rc;

	switch (index) {
	case VFIO_PCI_CONFIG_REGION_INDEX:
	case VFIO_PCI_BAR0_REGION_INDEX:
	case VFIO_PCI_BAR1_REGION_INDEX:
	case VFIO_PCI_BAR2_REGION_INDEX:
	case VFIO_PCI_BAR3_REGION_INDEX:
	case VFIO_PCI_BAR4_REGION_INDEX:
	case VFIO_PCI_BAR5_REGION_INDEX:
	case VFIO_PCI_VGA_REGION_INDEX:
	case VFIO_PCI_ROM_REGION_INDEX:
		break;
	default: {
		struct vfio_pci_core_device *vfio_pdev = &vidxd->vfio_pdev;
		struct device *dev = vdev->dev;

		dev_dbg(dev, "vendor specific region: %u\n", index);
		index -= VFIO_PCI_NUM_REGIONS;
		return vfio_pdev->region[index].ops->rw(vfio_pdev, buf, count, ppos, false);
	} /* end default */
	} /* end switch(index) */

	mutex_lock(&vidxd->dev_lock);
	while (count) {
		size_t filled;

		if (count >= 8 && !(*ppos % 8)) {
			u64 val;

			rc = idxd_vdcm_rw(vdev, (char *)&val, sizeof(val),
					  ppos, IDXD_VDCM_READ);
			if (rc <= 0)
				goto read_err;

			if (copy_to_user(buf, &val, sizeof(val)))
				goto read_err;

			filled = 8;
		} else if (count >= 4 && !(*ppos % 4)) {
			u32 val;

			rc = idxd_vdcm_rw(vdev, (char *)&val, sizeof(val),
					  ppos, IDXD_VDCM_READ);
			if (rc <= 0)
				goto read_err;

			if (copy_to_user(buf, &val, sizeof(val)))
				goto read_err;

			filled = 4;
		} else if (count >= 2 && !(*ppos % 2)) {
			u16 val;

			rc = idxd_vdcm_rw(vdev, (char *)&val, sizeof(val),
					  ppos, IDXD_VDCM_READ);
			if (rc <= 0)
				goto read_err;

			if (copy_to_user(buf, &val, sizeof(val)))
				goto read_err;

			filled = 2;
		} else {
			u8 val;

			rc = idxd_vdcm_rw(vdev, &val, sizeof(val), ppos,
					  IDXD_VDCM_READ);
			if (rc <= 0)
				goto read_err;

			if (copy_to_user(buf, &val, sizeof(val)))
				goto read_err;

			filled = 1;
		}

		count -= filled;
		done += filled;
		*ppos += filled;
		buf += filled;
	}

	mutex_unlock(&vidxd->dev_lock);
	return done;

 read_err:
	mutex_unlock(&vidxd->dev_lock);
	return -EFAULT;
}

static ssize_t idxd_vdcm_write(struct vfio_device *vdev, const char __user *buf, size_t count,
			       loff_t *ppos)
{
	struct vdcm_idxd *vidxd = vdev_to_vidxd(vdev);
	unsigned int index = VFIO_PCI_OFFSET_TO_INDEX(*ppos);
	unsigned int done = 0;
	int rc;

	switch (index) {
	case VFIO_PCI_CONFIG_REGION_INDEX:
	case VFIO_PCI_BAR0_REGION_INDEX:
	case VFIO_PCI_BAR1_REGION_INDEX:
	case VFIO_PCI_BAR2_REGION_INDEX:
	case VFIO_PCI_BAR3_REGION_INDEX:
	case VFIO_PCI_BAR4_REGION_INDEX:
	case VFIO_PCI_BAR5_REGION_INDEX:
	case VFIO_PCI_VGA_REGION_INDEX:
	case VFIO_PCI_ROM_REGION_INDEX:
		break;
	default: {
		struct vfio_pci_core_device *vfio_pdev = &vidxd->vfio_pdev;
		struct device *dev = vdev->dev;

		dev_dbg(dev, "vendor specific region: %u\n", index);
		index -= VFIO_PCI_NUM_REGIONS;
		return vfio_pdev->region[index].ops->rw(vfio_pdev, buf, count, ppos, true);
	} /* end default */
	} /* end switch(index) */

	mutex_lock(&vidxd->dev_lock);
	while (count) {
		size_t filled;

		if (count >= 8 && !(*ppos % 8)) {
			u64 val;

			if (copy_from_user(&val, buf, sizeof(val)))
				goto write_err;

			rc = idxd_vdcm_rw(vdev, (char *)&val, sizeof(val),
					  ppos, IDXD_VDCM_WRITE);
			if (rc <= 0)
				goto write_err;

			filled = 8;
		} else if (count >= 4 && !(*ppos % 4)) {
			u32 val;

			if (copy_from_user(&val, buf, sizeof(val)))
				goto write_err;

			rc = idxd_vdcm_rw(vdev, (char *)&val, sizeof(val),
					  ppos, IDXD_VDCM_WRITE);
			if (rc <= 0)
				goto write_err;

			filled = 4;
		} else if (count >= 2 && !(*ppos % 2)) {
			u16 val;

			if (copy_from_user(&val, buf, sizeof(val)))
				goto write_err;

			rc = idxd_vdcm_rw(vdev, (char *)&val,
					  sizeof(val), ppos, IDXD_VDCM_WRITE);
			if (rc <= 0)
				goto write_err;

			filled = 2;
		} else {
			u8 val;

			if (copy_from_user(&val, buf, sizeof(val)))
				goto write_err;

			rc = idxd_vdcm_rw(vdev, &val, sizeof(val),
					  ppos, IDXD_VDCM_WRITE);
			if (rc <= 0)
				goto write_err;

			filled = 1;
		}

		count -= filled;
		done += filled;
		*ppos += filled;
		buf += filled;
	}

	mutex_unlock(&vidxd->dev_lock);
	return done;

write_err:
	mutex_unlock(&vidxd->dev_lock);
	return -EFAULT;
}

static int idxd_vdcm_mmap(struct vfio_device *vdev, struct vm_area_struct *vma)
{
	unsigned int wq_idx, index;
	unsigned long req_size, pgoff = 0, offset;
	pgprot_t pg_prot;
	struct vdcm_idxd *vidxd = vdev_to_vidxd(vdev);
	struct idxd_device *idxd = vidxd->idxd;
	struct idxd_wq *wq = vidxd->wq;
	enum idxd_portal_prot virt_portal, phys_portal;
	phys_addr_t base = pci_resource_start(idxd->pdev, IDXD_WQ_BAR);
	struct device *dev = vdev->dev;

	if (!(vma->vm_flags & VM_SHARED))
		return -EINVAL;

	index = vma->vm_pgoff >> (VFIO_PCI_OFFSET_SHIFT - PAGE_SHIFT);
	if (index >= VFIO_PCI_NUM_REGIONS) {
		int regnum = index - VFIO_PCI_NUM_REGIONS;
		struct vfio_pci_region *region = vidxd->vfio_pdev.region + regnum;

		if (region && region->ops && region->ops->mmap &&
		    (region->flags & VFIO_REGION_INFO_FLAG_MMAP))
			return region->ops->mmap(&vidxd->vfio_pdev, region, vma);

		return -EINVAL;
	}

	pg_prot = vma->vm_page_prot;
	req_size = vma->vm_end - vma->vm_start;
	if (req_size > PAGE_SIZE)
		return -EINVAL;

	vma->vm_flags |= VM_DONTCOPY;

	offset = (vma->vm_pgoff << PAGE_SHIFT) &
		 ((1ULL << VFIO_PCI_OFFSET_SHIFT) - 1);

	wq_idx = offset >> (PAGE_SHIFT + 2);
	if (wq_idx >= 1) {
		dev_err(dev, "mapping invalid wq %d off %lx\n",
			wq_idx, offset);
		return -EINVAL;
	}

	/*
	 * Check and see if the guest wants to map to the limited or unlimited portal.
	 * The driver will allow mapping to unlimited portal only if the wq is a
	 * dedicated wq. Otherwise, it goes to limited.
	 */
	virt_portal = ((offset >> PAGE_SHIFT) & 0x3) == 1;
	phys_portal = IDXD_PORTAL_LIMITED;
	if (virt_portal == IDXD_PORTAL_UNLIMITED && wq_dedicated(wq))
		phys_portal = IDXD_PORTAL_UNLIMITED;

	/* We always map IMS portals to the guest */
	pgoff = (base + idxd_get_wq_portal_full_offset(wq->id, phys_portal,
						       IDXD_IRQ_IMS)) >> PAGE_SHIFT;

	dev_dbg(dev, "mmap %lx %lx %lx %lx\n", vma->vm_start, pgoff, req_size,
		pgprot_val(pg_prot));
	vma->vm_page_prot = pgprot_noncached(vma->vm_page_prot);
	vma->vm_pgoff = pgoff;

	return remap_pfn_range(vma, vma->vm_start, pgoff, req_size, pg_prot);
}

static void vidxd_vdcm_reset(struct vdcm_idxd *vidxd)
{
	vidxd_reset(vidxd, false);
}

static irqreturn_t idxd_vdcm_msix_handler(int irq, void *arg)
{
	struct vfio_pci_irq_ctx *ctx = (struct vfio_pci_irq_ctx *)arg;

	eventfd_signal(ctx->trigger, 1);
	return IRQ_HANDLED;
}

static void idxd_vdcm_free_irq (struct vfio_pci_core_device *vfio_pdev, int vector, int irq)
{
	u32 auxval;
	if (irq) {
		irq_bypass_unregister_producer(&vfio_pdev->ctx[vector].producer);
		free_irq(irq, &vfio_pdev->ctx[vector]);
		auxval = ims_ctrl_pasid_aux(0, false);
		irq_set_auxdata(irq, IMS_AUXDATA_CONTROL_WORD, auxval);
	}
	kfree(vfio_pdev->ctx[vector].name);
	vfio_pdev->ctx[vector].name = NULL;
}

static int idxd_vdcm_msix_set_vector_signal(struct vdcm_idxd *vidxd, int vector, int fd)
{
	struct vfio_pci_core_device *vfio_pdev = &vidxd->vfio_pdev;
	struct mdev_device *mdev = vidxd->ivdev.mdev;
	struct device *dev = mdev_dev(mdev);
	struct eventfd_ctx *trigger;
	char *name;
	u32 pasid, auxval;
	int irq, rc;
	u8 *bar0 = vidxd->bar0;
	u32 msix_perm;

	dev_dbg(dev, "%s: set signal %d fd: %d\n", __func__, vector, fd);

	if (vector < 0 || vector >= vfio_pdev->num_ctx) {
		dev_warn(dev, "%s out of boundary\n", __func__);
		return -EINVAL;
	}

	irq = vector ? dev_msi_irq_vector(dev, vector - 1) : 0;

	dev_dbg(dev, "%s: irq: %d\n", __func__, irq);

	if (vfio_pdev->ctx[vector].trigger) {
		if (irq)
			irq_bypass_unregister_producer(&vfio_pdev->ctx[vector].producer);

		eventfd_ctx_put(vfio_pdev->ctx[vector].trigger);

		if (fd < 0) {
			dev_dbg(dev, "%s: trigger already set, freeing\n", __func__);
			idxd_vdcm_free_irq(vfio_pdev, vector, irq);
			return 0;
		}
		dev_dbg(dev, "%s: trigger already set, changing\n", __func__);
		trigger = eventfd_ctx_fdget(fd);
		if (IS_ERR(trigger)) {
			dev_dbg(dev, "%s: trigger change failed, freeing\n", __func__);
			idxd_vdcm_free_irq(vfio_pdev, vector, irq);
			vfio_pdev->ctx[vector].trigger = NULL;
			return PTR_ERR(trigger);
		}
		vfio_pdev->ctx[vector].trigger = trigger;

		if (irq) {
			/* Update IRQ Bypass Setting */
			vfio_pdev->ctx[vector].producer.token = trigger;
			vfio_pdev->ctx[vector].producer.irq = irq;
			rc = irq_bypass_register_producer(&vfio_pdev->ctx[vector].producer);
			if (unlikely(rc)) {
				dev_warn(dev, "irq bypass producer (token %p) registration fails: %d\n",
					vfio_pdev->ctx[vector].producer.token, rc);
				vfio_pdev->ctx[vector].producer.token = NULL;
			}
			dev_dbg(dev, "%s: updated irq %d\n", __func__, irq);
		}
		return 0;
	}

	if (fd < 0)
		return 0;

	name = kasprintf(GFP_KERNEL, "vfio-dev-ims[%d](%s)", vector, dev_name(dev));
	if (!name)
		return -ENOMEM;

	trigger = eventfd_ctx_fdget(fd);
	if (IS_ERR(trigger)) {
		kfree(name);
		return PTR_ERR(trigger);
	}

	vfio_pdev->ctx[vector].name = name;
	vfio_pdev->ctx[vector].trigger = trigger;

	dev_dbg(dev, "%s: trigger: %px\n", __func__, trigger);

	if (!irq) {
		dev_dbg(dev, "Mediated vector 0 set\n");
		return 0;
	}

	/*
	 * This only points to MSIX entry 1, which is fine for now.
	 */
	msix_perm = *(u32 *)(bar0 + VIDXD_MSIX_PERM_OFFSET + 8 * vector);
	dev_dbg(dev, "MSIX PERM: %#x\n", msix_perm);
	if (!(msix_perm & BIT(3))) {
		rc = idxd_mdev_get_pasid(mdev, &pasid);
		if (rc < 0) {
			dev_warn(dev, "%s unable to get pasid, failing\n", __func__);
			goto err;
		}

		dev_dbg(dev, "%s: pasid: %d\n", __func__, pasid);
	} else {
		u32 gpasid;

		gpasid = (msix_perm & GENMASK(31, 12)) >> 12;
		rc = idxd_mdev_get_host_pasid(vidxd->ivdev.mdev, gpasid, &pasid);
		if (rc < 0) {
			dev_warn(dev, "%s guest pasid %u translate failure\n", __func__, gpasid);
			goto err;
		}
		dev_dbg(dev, "%s: guest pasid: %u host pasid: %u\n",
			__func__, gpasid, pasid);
	}

	auxval = ims_ctrl_pasid_aux(pasid, true);
	rc = irq_set_auxdata(irq, IMS_AUXDATA_CONTROL_WORD, auxval);
	if (rc < 0) {
		dev_warn(dev, "%s: set IMS aux data failed: %d\n", __func__, rc);
		goto err;
	}

	rc = request_irq(irq, idxd_vdcm_msix_handler, 0, name, &vfio_pdev->ctx[vector]);
	if (rc < 0) {
		dev_warn(dev, "%s request_irq() failed\n", __func__);
		goto irq_err;
	}

	vfio_pdev->ctx[vector].producer.token = trigger;
	vfio_pdev->ctx[vector].producer.irq = irq;
	rc = irq_bypass_register_producer(&vfio_pdev->ctx[vector].producer);
	if (unlikely(rc)) {
		dev_warn(dev, "irq bypass producer (token %p) registration fails: %d\n",
			vfio_pdev->ctx[vector].producer.token, rc);
		vfio_pdev->ctx[vector].producer.token = NULL;
	}

	dev_dbg(dev, "%s: irq %d set\n", __func__, irq);

	return 0;

 irq_err:
	auxval = ims_ctrl_pasid_aux(0, false);
	irq_set_auxdata(irq, IMS_AUXDATA_CONTROL_WORD, auxval);
 err:
	kfree(name);
	vfio_pdev->ctx[vector].name = NULL;
	eventfd_ctx_put(trigger);
	vfio_pdev->ctx[vector].trigger = NULL;
	return rc;
}

static int idxd_vdcm_msix_set_vector_signals(struct vdcm_idxd *vidxd, u32 start,
					     u32 count, int *fds)
{
	struct vfio_pci_core_device *vfio_pdev = &vidxd->vfio_pdev;
	int i, j, rc = 0;
	struct mdev_device *mdev = vidxd->ivdev.mdev;
	struct device *dev = mdev_dev(mdev);

	if (start >= vfio_pdev->num_ctx || start + count > vfio_pdev->num_ctx) {
		dev_warn(dev, "%s out of boundary\n", __func__);
		return -EINVAL;
	}

	for (i = 0, j = start; i < count && !rc; i++, j++) {
		int fd = fds ? fds[i] : -1;

		dev_dbg(dev, "%s: %s signal %d, fd: %d\n",
			__func__, (fd == -1) ? "unset" : "set", j, fd);
		rc = idxd_vdcm_msix_set_vector_signal(vidxd, j, fd);
	}

	if (rc) {
		dev_warn(dev, "%s: set signal failed, unwind\n", __func__);
		for (--j; j >= (int)start; j--)
			idxd_vdcm_msix_set_vector_signal(vidxd, j, -1);
	}

	return rc;
}

static int idxd_vdcm_msix_enable(struct vdcm_idxd *vidxd, int nvec)
{
	struct mdev_device *mdev = vidxd->ivdev.mdev;
	struct device *dev = mdev_dev(mdev);
	struct vfio_pci_core_device *vfio_pdev = &vidxd->vfio_pdev;
	int rc;

	dev_dbg(dev, "%s: nvec: %d\n", __func__, nvec);

	/* There should be at least 1 vectors for idxd */
	if (nvec < 1)
		return -EINVAL;

	dev_dbg(dev, "%s: allocating\n", __func__);
	vfio_pdev->ctx = kcalloc(nvec, sizeof(struct vfio_pci_irq_ctx), GFP_KERNEL);
	if (!vfio_pdev->ctx) {
		dev_warn(dev, "%s: failed to alloc VFIO irq context\n", __func__);
		return -ENOMEM;
	}

	if (nvec > 1) {
		dev_dbg(dev, "%s: allocate %d IMS\n", __func__, nvec - 1);
		rc = msi_domain_alloc_irqs(dev_get_msi_domain(dev), dev, nvec - 1);
		if (rc < 0) {
			dev_warn(dev, "%s failed to allocate irq on IMS domain: %d\n",
				 __func__, rc);
			kfree(vfio_pdev->ctx);
			return rc;
		}
	}

	vfio_pdev->num_ctx = nvec;
	vfio_pdev->irq_type = VFIO_PCI_MSIX_IRQ_INDEX;
	return 0;
}

static int idxd_vdcm_msix_disable(struct vdcm_idxd *vidxd)
{
	struct mdev_device *mdev = vidxd->ivdev.mdev;
	struct device *dev = mdev_dev(mdev);
	struct irq_domain *irq_domain;
	struct vfio_pci_core_device *vfio_pdev = &vidxd->vfio_pdev;

	/* Check if somebody already disabled it */
	if (vfio_pdev->num_ctx == 0)
		return 0;

	idxd_vdcm_msix_set_vector_signals(vidxd, 0, vfio_pdev->num_ctx, NULL);
	irq_domain = dev_get_msi_domain(dev);
	if (irq_domain)
		msi_domain_free_irqs(irq_domain, dev);
	kfree(vfio_pdev->ctx);
	vfio_pdev->num_ctx = 0;
	vfio_pdev->irq_type = VFIO_PCI_NUM_IRQS;
	return 0;
}

static int idxd_vdcm_set_msix_trigger(struct vdcm_idxd *vidxd, u32 index, u32 start,
				      u32 count, u32 flags, void *data)
{
	struct vfio_pci_core_device *vfio_pdev = &vidxd->vfio_pdev;
	int rc, i;

	struct mdev_device *mdev = vidxd->ivdev.mdev;
	struct device *dev = mdev_dev(mdev);

	dev_dbg(dev, "%s(index: %d start: %d count: %d flags: %d data: %px\n",
		__func__, index, start, count, flags, data);

	if (count > VIDXD_MAX_MSIX_VECS)
		count = VIDXD_MAX_MSIX_VECS;

	if (!count && (flags & VFIO_IRQ_SET_DATA_NONE)) {
		dev_dbg(dev, "%s disabling\n", __func__);
		idxd_vdcm_msix_disable(vidxd);
		return 0;
	}

	if (flags & VFIO_IRQ_SET_DATA_EVENTFD) {
		int *fds = data;

		if (vfio_pdev->irq_type == index) {
			dev_dbg(dev, "%s straight set signal\n", __func__);
			return idxd_vdcm_msix_set_vector_signals(vidxd, start, count, fds);
		}

		rc = idxd_vdcm_msix_enable(vidxd, start + count);
		if (rc < 0)
			return rc;

		rc = idxd_vdcm_msix_set_vector_signals(vidxd, start, count, fds);
		if (rc < 0)
			idxd_vdcm_msix_disable(vidxd);

		return rc;
	}

	if (start + count > VIDXD_MAX_MSIX_VECS)
		return -EINVAL;

	for (i = start; i < start + count; i++) {
		if (!vfio_pdev->ctx[i].trigger)
			continue;
		if (flags & VFIO_IRQ_SET_DATA_NONE) {
			eventfd_signal(vfio_pdev->ctx[i].trigger, 1);
		} else if (flags & VFIO_IRQ_SET_DATA_BOOL) {
			u8 *bools = data;

			if (bools[i - start])
				eventfd_signal(vfio_pdev->ctx[i].trigger, 1);
		}
	}
	return 0;
}


static int idxd_vdcm_set_ctx_trigger_single(struct eventfd_ctx **ctx,
					    unsigned int count, u32 flags, void *data)
{
	/* DATA_NONE/DATA_BOOL enables loopback testing */
	if (flags & VFIO_IRQ_SET_DATA_NONE) {
		if (*ctx) {
			if (count) {
				eventfd_signal(*ctx, 1);
			} else {
				eventfd_ctx_put(*ctx);
				*ctx = NULL;
			}
			return 0;
		}
	} else if (flags & VFIO_IRQ_SET_DATA_BOOL) {
		u8 trigger;

		if (!count)
			return -EINVAL;

		if (!data)
			return -EINVAL;

		trigger = *(u8 *)data;
		if (trigger && *ctx)
			eventfd_signal(*ctx, 1);

		return 0;
	} else if (flags & VFIO_IRQ_SET_DATA_EVENTFD) {
		s32 fd;

		if (!count)
			return -EINVAL;

		fd = *(s32 *)data;
		if (fd == -1) {
			if (*ctx)
				eventfd_ctx_put(*ctx);
			*ctx = NULL;
		} else if (fd >= 0) {
			struct eventfd_ctx *efdctx;

			efdctx = eventfd_ctx_fdget(fd);
			if (IS_ERR(efdctx))
				return PTR_ERR(efdctx);

			if (*ctx)
				eventfd_ctx_put(*ctx);

			*ctx = efdctx;
		}
		return 0;
	}

	return -EINVAL;
}

static int idxd_vdcm_set_req_trigger(struct mdev_device *mdev, unsigned int index,
				    unsigned int start, unsigned int count,
				    u32 flags, void *data)
{
	if (index != VFIO_PCI_REQ_IRQ_INDEX || start != 0 || count > 1)
		return -EINVAL;

	return idxd_vdcm_set_ctx_trigger_single(&mdev->req_trigger, count, flags, data);
}

static int idxd_vdcm_set_irqs(struct vdcm_idxd *vidxd, uint32_t flags,
			      unsigned int index, unsigned int start,
			      unsigned int count, void *data)
{
	struct mdev_device *mdev = vidxd->ivdev.mdev;
	struct device *dev = mdev_dev(mdev);
	struct vfio_pci_core_device *vfio_pdev = &vidxd->vfio_pdev;

	dev_dbg(dev, "%s: flags: %#x index: %#x, start: %#x, count: %#x, data: %px\n",
		__func__, flags, index, start, count, data);

	switch (index) {
	case VFIO_PCI_MSIX_IRQ_INDEX:
		switch (flags & VFIO_IRQ_SET_ACTION_TYPE_MASK) {
		case VFIO_IRQ_SET_ACTION_MASK:
		case VFIO_IRQ_SET_ACTION_UNMASK:
			break;
		case VFIO_IRQ_SET_ACTION_TRIGGER:
			return idxd_vdcm_set_msix_trigger(vidxd, index, start, count, flags, data);
		}
		break;
	case VFIO_PCI_REQ_IRQ_INDEX:
		switch (flags & VFIO_IRQ_SET_ACTION_TYPE_MASK) {
		case VFIO_IRQ_SET_ACTION_TRIGGER:
			return idxd_vdcm_set_req_trigger(mdev, index, start, count, flags, data);
		}
		break;
	default:
		switch (flags & VFIO_IRQ_SET_ACTION_TYPE_MASK) {
		case VFIO_IRQ_SET_ACTION_TRIGGER:
			return vfio_pci_set_ext_irq_trigger(vfio_pdev, index, start,
							    count, flags, data);
		}
		break;
	}

	return -ENOTTY;
}

static long idxd_vdcm_ioctl(struct vfio_device *vdev, unsigned int cmd,
			    unsigned long arg)
{
	struct vdcm_idxd *vidxd = vdev_to_vidxd(vdev);
	unsigned long minsz;
	int rc = -EINVAL;
	struct device *dev = vdev->dev;
	struct vfio_pci_core_device *vfio_pdev = &vidxd->vfio_pdev;
	struct mdev_device *mdev = vidxd->ivdev.mdev;

	dev_dbg(dev, "vidxd %p ioctl, cmd: %d\n", vidxd, cmd);

	mutex_lock(&vidxd->dev_lock);
	if (cmd == VFIO_DEVICE_GET_INFO) {
		struct vfio_device_info info;

		minsz = offsetofend(struct vfio_device_info, num_irqs);

		if (copy_from_user(&info, (void __user *)arg, minsz)) {
			rc = -EFAULT;
			goto out;
		}

		if (info.argsz < minsz) {
			rc = -EINVAL;
			goto out;
		}

		info.flags = VFIO_DEVICE_FLAGS_PCI;
		info.flags |= VFIO_DEVICE_FLAGS_RESET;
		info.num_regions = VFIO_PCI_NUM_REGIONS + vfio_pdev->num_regions;
		info.num_irqs = VFIO_PCI_NUM_IRQS + vfio_pdev->num_ext_irqs;

		if (copy_to_user((void __user *)arg, &info, minsz))
			rc = -EFAULT;
		else
			rc = 0;
		goto out;
	} else if (cmd == VFIO_DEVICE_GET_REGION_INFO) {
		struct vfio_region_info info;
		struct vfio_info_cap caps = { .buf = NULL, .size = 0 };
		struct vfio_region_info_cap_sparse_mmap *sparse = NULL;
		size_t size;
		int nr_areas = 1;
		int cap_type_id = 0;

		minsz = offsetofend(struct vfio_region_info, offset);

		if (copy_from_user(&info, (void __user *)arg, minsz)) {
			rc = -EFAULT;
			goto out;
		}

		if (info.argsz < minsz) {
			rc = -EINVAL;
			goto out;
		}

		switch (info.index) {
		case VFIO_PCI_CONFIG_REGION_INDEX:
			info.offset = VFIO_PCI_INDEX_TO_OFFSET(info.index);
			info.size = VIDXD_MAX_CFG_SPACE_SZ;
			info.flags = VFIO_REGION_INFO_FLAG_READ | VFIO_REGION_INFO_FLAG_WRITE;
			break;
		case VFIO_PCI_BAR0_REGION_INDEX:
			info.offset = VFIO_PCI_INDEX_TO_OFFSET(info.index);
			info.size = vidxd->bar_size[info.index];
			if (!info.size) {
				info.flags = 0;
				break;
			}

			info.flags = VFIO_REGION_INFO_FLAG_READ | VFIO_REGION_INFO_FLAG_WRITE;
			break;
		case VFIO_PCI_BAR1_REGION_INDEX:
			info.offset = VFIO_PCI_INDEX_TO_OFFSET(info.index);
			info.size = 0;
			info.flags = 0;
			break;
		case VFIO_PCI_BAR2_REGION_INDEX:
			info.offset = VFIO_PCI_INDEX_TO_OFFSET(info.index);
			info.flags = VFIO_REGION_INFO_FLAG_CAPS | VFIO_REGION_INFO_FLAG_MMAP |
				     VFIO_REGION_INFO_FLAG_READ | VFIO_REGION_INFO_FLAG_WRITE |
				     VFIO_REGION_INFO_FLAG_DYNAMIC_TRAP;
			info.size = vidxd->bar_size[1];

			/*
			 * Every WQ has two areas for unlimited and limited
			 * MSI-X portals. IMS portals are not reported. For shared
			 * WQ, we will only allow limited portal.
			 */
			nr_areas = wq_dedicated(vidxd->wq) ? 2 : 1;

			size = sizeof(*sparse) + (nr_areas * sizeof(*sparse->areas));
			sparse = kzalloc(size, GFP_KERNEL);
			if (!sparse) {
				rc = -ENOMEM;
				goto out;
			}

			sparse->header.id = VFIO_REGION_INFO_CAP_SPARSE_MMAP;
			sparse->header.version = 1;
			sparse->nr_areas = nr_areas;
			cap_type_id = VFIO_REGION_INFO_CAP_SPARSE_MMAP;

			/* Unlimited portal */
			if (wq_dedicated(vidxd->wq)) {
				sparse->areas[0].offset = 0;
				sparse->areas[0].size = PAGE_SIZE;
				sparse->areas[1].offset = PAGE_SIZE;
				sparse->areas[1].size = PAGE_SIZE;
			} else {
			/* Limited portal */
				sparse->areas[0].offset = PAGE_SIZE;
				sparse->areas[0].size = PAGE_SIZE;
			}

			break;

		case VFIO_PCI_BAR3_REGION_INDEX ... VFIO_PCI_BAR5_REGION_INDEX:
			info.offset = VFIO_PCI_INDEX_TO_OFFSET(info.index);
			info.size = 0;
			info.flags = 0;
			dev_dbg(dev, "get region info bar:%d\n", info.index);
			break;

		case VFIO_PCI_ROM_REGION_INDEX:
		case VFIO_PCI_VGA_REGION_INDEX:
			dev_dbg(dev, "get region info index:%d\n", info.index);
			break;
		default: {
			struct vfio_region_info_cap_type cap_type = {
				.header.id = VFIO_REGION_INFO_CAP_TYPE,
				.header.version = 1,
			};
			int i;

			if (info.index >= VFIO_PCI_NUM_REGIONS + vfio_pdev->num_regions) {
				rc = -EINVAL;
				goto out;
			}

			info.index = array_index_nospec(info.index,
							VFIO_PCI_NUM_REGIONS +
							vfio_pdev->num_regions);
			i = info.index - VFIO_PCI_NUM_REGIONS;

			info.offset = VFIO_PCI_INDEX_TO_OFFSET(info.index);
			info.size = vfio_pdev->region[i].size;
			info.flags = vfio_pdev->region[i].flags;

			cap_type.type = vfio_pdev->region[i].type;
			cap_type.subtype = vfio_pdev->region[i].subtype;

			rc = vfio_info_add_capability(&caps, &cap_type.header, sizeof(cap_type));
			if (rc)
				goto out;

			if (vfio_pdev->region[i].ops->add_capability) {
				rc = vfio_pdev->region[i].ops->add_capability(vfio_pdev,
									  &vfio_pdev->region[i],
									  &caps);
				if (rc)
					goto out;
			}
		} /* default */
		} /* info.index switch */

		if ((info.flags & VFIO_REGION_INFO_FLAG_CAPS) && sparse) {
			if (cap_type_id == VFIO_REGION_INFO_CAP_SPARSE_MMAP) {
				rc = vfio_info_add_capability(&caps, &sparse->header,
							      sizeof(*sparse) + (sparse->nr_areas *
							      sizeof(*sparse->areas)));
				kfree(sparse);
				if (rc)
					goto out;
			}
		}

		if (caps.size) {
			info.flags |= VFIO_REGION_INFO_FLAG_CAPS;
			if (info.argsz < sizeof(info) + caps.size) {
				info.argsz = sizeof(info) + caps.size;
				info.cap_offset = 0;
			} else {
				vfio_info_cap_shift(&caps, sizeof(info));
				if (copy_to_user((void __user *)arg + sizeof(info),
						 caps.buf, caps.size)) {
					kfree(caps.buf);
					rc = -EFAULT;
					goto out;
				}
				info.cap_offset = sizeof(info);
			}

			kfree(caps.buf);
		}
		if (copy_to_user((void __user *)arg, &info, minsz))
			rc = -EFAULT;
		else
			rc = 0;
		goto out;
	} else if (cmd == VFIO_DEVICE_GET_IRQ_INFO) {
		struct vfio_irq_info info;
		struct vfio_info_cap caps = {
			.buf = NULL,
			.size = 0
		};
		unsigned long capsz;

		minsz = offsetofend(struct vfio_irq_info, count);
		capsz = offsetofend(struct vfio_irq_info, cap_offset);

		if (copy_from_user(&info, (void __user *)arg, minsz)) {
			rc = -EFAULT;
			goto out;
		}

		if (info.argsz < minsz ||
		    info.index >= VFIO_PCI_NUM_IRQS + vfio_pdev->num_ext_irqs) {
			rc = -EINVAL;
			goto out;
		}

		if (info.argsz >= capsz)
			minsz = capsz;

		info.flags = VFIO_IRQ_INFO_EVENTFD;

		switch (info.index) {
		case VFIO_PCI_INTX_IRQ_INDEX:
		case VFIO_PCI_MSI_IRQ_INDEX:
		case VFIO_PCI_ERR_IRQ_INDEX:
			rc = -EINVAL;
			goto out;
		case VFIO_PCI_MSIX_IRQ_INDEX:
		case VFIO_PCI_REQ_IRQ_INDEX:
			info.flags |= VFIO_IRQ_INFO_NORESIZE;
			break;
		default: {
			struct vfio_irq_info_cap_type cap_type = {
				.header.id = VFIO_IRQ_INFO_CAP_TYPE,
				.header.version = 1
			};
			int i;

			if (info.index >= VFIO_PCI_NUM_IRQS + vfio_pdev->num_ext_irqs)
				return -EINVAL;
			info.index = array_index_nospec(info.index,
							VFIO_PCI_NUM_IRQS + vfio_pdev->num_ext_irqs);
			i = info.index - VFIO_PCI_NUM_IRQS;

			info.flags = vfio_pdev->ext_irqs[i].flags;
			cap_type.type = vfio_pdev->ext_irqs[i].type;
			cap_type.subtype = vfio_pdev->ext_irqs[i].subtype;

			rc = vfio_info_add_capability(&caps, &cap_type.header, sizeof(cap_type));
			if (rc)
				goto out;
			break;
		}
		} /* switch(info.index) */

		info.count = idxd_vdcm_get_irq_count(mdev, info.index);
		if (caps.size) {
			info.flags |= VFIO_IRQ_INFO_FLAG_CAPS;
			if (info.argsz < sizeof(info) + caps.size) {
				info.argsz = sizeof(info) + caps.size;
				info.cap_offset = 0;
			} else {
				vfio_info_cap_shift(&caps, sizeof(info));
				if (copy_to_user((void __user *)arg + sizeof(info), caps.buf,
						 caps.size)) {
					kfree(caps.buf);
					return -EFAULT;
				}
				info.cap_offset = sizeof(info);
			}
			kfree(caps.buf);
		}

		rc = copy_to_user((void __user *)arg, &info, minsz);
		rc = rc ? -EFAULT : 0;
		goto out;
	} else if (cmd == VFIO_DEVICE_SET_IRQS) {
		struct vfio_irq_set hdr;
		u8 *data = NULL;
		size_t data_size = 0;
		int max;

		minsz = offsetofend(struct vfio_irq_set, count);

		if (copy_from_user(&hdr, (void __user *)arg, minsz)) {
			rc = -EFAULT;
			goto out;
		}

		max = idxd_vdcm_get_irq_count(mdev, hdr.index);
		rc = vfio_set_irqs_validate_and_prepare(&hdr, max,
							VFIO_PCI_NUM_IRQS +
							vfio_pdev->num_ext_irqs,
							&data_size);
		if (rc) {
			dev_err(dev, "intel:vfio_set_irqs_validate_and_prepare failed\n");
			goto out;
		}

		if (data_size) {
			data = memdup_user((void __user *)(arg + minsz), data_size);
			if (IS_ERR(data)) {
				rc = PTR_ERR(data);
				goto out;
			}
		}
		mutex_lock(&vidxd->vfio_pdev.igate);
		rc = idxd_vdcm_set_irqs(vidxd, hdr.flags, hdr.index, hdr.start, hdr.count, data);
		mutex_unlock(&vidxd->vfio_pdev.igate);
		kfree(data);
		goto out;
	} else if (cmd == VFIO_DEVICE_RESET) {
		vidxd_vdcm_reset(vidxd);
	}

 out:
	mutex_unlock(&vidxd->dev_lock);
	return rc;
}

static void idxd_vdcm_mdev_request(struct vfio_device *vdev, unsigned int count)
{
	struct vdcm_idxd *vidxd = vdev_to_vidxd(vdev);
	struct vfio_pci_core_device *vfio_pdev = &vidxd->vfio_pdev;
	struct mdev_device *mdev = vidxd->ivdev.mdev;

	mutex_lock(&vfio_pdev->igate);
	if (mdev->req_trigger) {
		if (!(count % 10))
			dev_info_ratelimited(mdev_dev(mdev),
					     "Relaying device request to user (#%u)\n",
					     count);
		eventfd_signal(mdev->req_trigger, 1);
	} else if (count == 0) {
		dev_warn(mdev_dev(mdev),
			 "No device request channel registered, blocked until released by user\n");
	}

	mutex_unlock(&vfio_pdev->igate);
}

static ssize_t name_show(struct mdev_type *mtype, struct mdev_type_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "%s\n", idxd_mdev_types[mtype_get_type_group_id(mtype)].name);
}
static MDEV_TYPE_ATTR_RO(name);

static int find_available_mdev_instances(struct idxd_device *idxd, struct vdcm_idxd_type *type)
{
	int count = 0, i;
	unsigned long flags;

	switch (type->type) {
	case IDXD_MDEV_TYPE_DSA_1_DWQ:
	case IDXD_MDEV_TYPE_DSA_1_SWQ:
		if (idxd->data->type != IDXD_TYPE_DSA)
			return 0;
		break;
	case IDXD_MDEV_TYPE_IAX_1_DWQ:
	case IDXD_MDEV_TYPE_IAX_1_SWQ:
		if (idxd->data->type != IDXD_TYPE_IAX)
			return 0;
		break;
	default:
		return 0;
	}

	spin_lock_irqsave(&idxd->dev_lock, flags);
	for (i = 0; i < idxd->max_wqs; i++) {
		struct idxd_wq *wq;

		wq = idxd->wqs[i];

		if (wq->state != IDXD_WQ_ENABLED)
			continue;

		if (!is_idxd_wq_mdev(wq))
			continue;

		switch (type->type) {
		case IDXD_MDEV_TYPE_DSA_1_DWQ:
		case IDXD_MDEV_TYPE_IAX_1_DWQ:
			if (wq_dedicated(wq) && !idxd_wq_refcount(wq))
				count++;
			break;
		case IDXD_MDEV_TYPE_DSA_1_SWQ:
		case IDXD_MDEV_TYPE_IAX_1_SWQ:
			if (!wq_dedicated(wq))
				count++;
			break;
		default:
			return 0;
		}
	}
	spin_unlock_irqrestore(&idxd->dev_lock, flags);

	return count;
}

static ssize_t available_instances_show(struct mdev_type *mtype,
					struct mdev_type_attribute *attr,
					char *buf)
{
	struct device *dev = mtype_get_parent_dev(mtype);
	struct idxd_device *idxd = dev_get_drvdata(dev);
	int count;
	struct vdcm_idxd_type *type;

	type = &idxd_mdev_types[mtype_get_type_group_id(mtype)];
	count = find_available_mdev_instances(idxd, type);

	return sprintf(buf, "%d\n", count);
}
static MDEV_TYPE_ATTR_RO(available_instances);

static ssize_t device_api_show(struct mdev_type *mtype, struct mdev_type_attribute *attr,
			       char *buf)
{
	return sprintf(buf, "%s\n", VFIO_DEVICE_API_PCI_STRING);
}
static MDEV_TYPE_ATTR_RO(device_api);

static struct attribute *idxd_mdev_types_attrs[] = {
	&mdev_type_attr_name.attr,
	&mdev_type_attr_device_api.attr,
	&mdev_type_attr_available_instances.attr,
	NULL,
};

static struct attribute_group idxd_mdev_type_dsa_group0 = {
	.name = idxd_dsa_1dwq_name,
	.attrs = idxd_mdev_types_attrs,
};

static struct attribute_group idxd_mdev_type_iax_group0 = {
	.name = idxd_iax_1dwq_name,
	.attrs = idxd_mdev_types_attrs,
};

static struct attribute_group idxd_mdev_type_dsa_group1 = {
	.name = idxd_dsa_1swq_name,
	.attrs = idxd_mdev_types_attrs,
};

static struct attribute_group idxd_mdev_type_iax_group1 = {
	.name = idxd_iax_1swq_name,
	.attrs = idxd_mdev_types_attrs,
};

static struct attribute_group *idxd_mdev_type_groups[] = {
	&idxd_mdev_type_dsa_group0,
	&idxd_mdev_type_iax_group0,
	&idxd_mdev_type_dsa_group1,
	&idxd_mdev_type_iax_group1,
	NULL,
};

static const struct vfio_device_ops idxd_mdev_ops = {
	.name = "vfio-mdev",
	.open_device = idxd_vdcm_open,
	.close_device = idxd_vdcm_close,
	.read = idxd_vdcm_read,
	.write = idxd_vdcm_write,
	.mmap = idxd_vdcm_mmap,
	.ioctl = idxd_vdcm_ioctl,
	.request = idxd_vdcm_mdev_request,
};

static struct mdev_driver idxd_vdcm_driver = {
	.driver = {
		.name = "idxd-mdev",
		.owner = THIS_MODULE,
		.mod_name = KBUILD_MODNAME,
	},
	.probe = idxd_vdcm_probe,
	.remove = idxd_vdcm_remove,
};

static const struct mdev_parent_ops idxd_parent_ops = {
	.owner = THIS_MODULE,
	.device_driver = &idxd_vdcm_driver,
	.supported_type_groups = idxd_mdev_type_groups,
};

static int idxd_mdev_drv_probe(struct idxd_dev *idxd_dev)
{
	struct device *dev = &idxd_dev->conf_dev;
	struct idxd_wq *wq = idxd_dev_to_wq(idxd_dev);
	struct idxd_device *idxd = wq->idxd;
	int rc;

	if (idxd->state != IDXD_DEV_ENABLED)
		return -ENXIO;

	mutex_lock(&wq->wq_lock);
	wq->type = IDXD_WQT_MDEV;

	rc = __drv_enable_wq(wq);
	mutex_unlock(&wq->wq_lock);
	if (rc < 0)
		return rc;

	mutex_lock(&idxd->kref_lock);
	/*
	 * If kref == 1, that means there are no mdev clients and mdev has
	 * not been registered.
	 */
	if (!idxd->mdev_host_init) {
		kref_init(&idxd->mdev_kref);
		rc = idxd_mdev_host_init(idxd, &idxd_parent_ops);
		if (rc < 0) {
			mutex_unlock(&idxd->kref_lock);
			drv_disable_wq(wq);
			dev_warn(dev, "mdev device init failed!\n");
			return -ENXIO;
		}
	} else {
		kref_get(&idxd->mdev_kref);
	}
	mutex_unlock(&idxd->kref_lock);

	get_device(dev);
	dev_info(dev, "wq %s enabled\n", dev_name(dev));
	return 0;
}

static void idxd_mdev_drv_remove(struct idxd_dev *idxd_dev)
{
	struct device *dev = &idxd_dev->conf_dev;
	struct idxd_wq *wq = idxd_dev_to_wq(idxd_dev);
	struct idxd_device *idxd = wq->idxd;

	mutex_lock(&wq->wq_lock);
	__drv_disable_wq(wq);

	if (wq->state == IDXD_WQ_LOCKED)
		wq->state = IDXD_WQ_DISABLED;
	mutex_unlock(&wq->wq_lock);

	mutex_lock(&idxd->kref_lock);
	if (idxd->mdev_host_init) {
		kref_put(&idxd->mdev_kref, idxd_mdev_host_release);

		/* kref init at 1, when it hits 1, no more devices and we can release */
		if (kref_read(&idxd->mdev_kref) == 1)
			kref_put(&idxd->mdev_kref, idxd_mdev_host_release);
	}
	mutex_unlock(&idxd->kref_lock);
	put_device(dev);
	dev_info(dev, "wq %s disabled\n", dev_name(dev));
}

static struct idxd_device_ops mdev_wq_ops = {
	.notify_error = idxd_wq_vidxd_send_errors,
};

static enum idxd_dev_type dev_types[] = {
	IDXD_DEV_WQ,
	IDXD_DEV_NONE,
};

static struct idxd_device_driver idxd_mdev_driver = {
	.probe = idxd_mdev_drv_probe,
	.remove = idxd_mdev_drv_remove,
	.name = "mdev",
	.type = dev_types,
	.ops = &mdev_wq_ops,
};

static int __init idxd_mdev_init(void)
{
	int rc;

	rc = idxd_driver_register(&idxd_mdev_driver);
	if (rc < 0)
		return rc;

	rc = mdev_register_driver(&idxd_vdcm_driver);
	if (rc < 0) {
		idxd_driver_unregister(&idxd_mdev_driver);
		return rc;
	}

	return 0;
}

static void __exit idxd_mdev_exit(void)
{
	mdev_unregister_driver(&idxd_vdcm_driver);
	idxd_driver_unregister(&idxd_mdev_driver);
}

module_init(idxd_mdev_init);
module_exit(idxd_mdev_exit);

MODULE_IMPORT_NS(IDXD);
MODULE_SOFTDEP("pre: idxd");
MODULE_SOFTDEP("pre: mdev");
MODULE_LICENSE("GPL v2");
MODULE_AUTHOR("Intel Corporation");
MODULE_ALIAS_IDXD_DEVICE(0);
