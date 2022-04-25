/* SPDX-License-Identifier: GPL-2.0 */
/* Copyright(c) 2020 Intel Corporation. All rights rsvd. */

#ifndef _IDXD_MDEV_H_
#define _IDXD_MDEV_H_

#include <linux/vfio.h>
#include <linux/vfio_pci_core.h>

/* two 64-bit BARs implemented */
#define VIDXD_MAX_BARS 2
#define VIDXD_MAX_CFG_SPACE_SZ 4096
#define VIDXD_MAX_MMIO_SPACE_SZ 8192
#define VIDXD_MSIX_TBL_SZ_OFFSET 0x42
#define VIDXD_CAP_CTRL_SZ 0x100
#define VIDXD_GRP_CTRL_SZ 0x100
#define VIDXD_WQ_CTRL_SZ 0x100
#define VIDXD_WQ_OCPY_INT_SZ 0x20
#define VIDXD_MSIX_TBL_SZ 0x90
#define VIDXD_MSIX_PERM_TBL_SZ 0x48

#define VIDXD_MSIX_TABLE_OFFSET 0x600
#define VIDXD_MSIX_PERM_OFFSET 0x300
#define VIDXD_GRPCFG_OFFSET 0x400
#define VIDXD_WQCFG_OFFSET 0x500
#define VIDXD_IMS_OFFSET 0x1000

#define VIDXD_BAR0_SIZE  0x2000
#define VIDXD_BAR2_SIZE  0x2000
#define VIDXD_MAX_MSIX_ENTRIES  (VIDXD_MSIX_TBL_SZ / 0x10)
#define VIDXD_MAX_WQS	1
#define VIDXD_MAX_MSIX_VECS	2

#define VIDXD_ATS_OFFSET 0x100
#define VIDXD_PRS_OFFSET 0x110
#define VIDXD_PASID_OFFSET 0x120
#define VIDXD_MSIX_PBA_OFFSET 0x700

#define VIDXD_STATE_BUFFER_SIZE (4 * PAGE_SIZE)
#define VIDXD_MAX_INTS 65536

struct ioasid_mm_entry {
	struct mm_struct *mm;
	struct list_head node;
};

#define IDXD_DESC_SIZE sizeof(struct dsa_hw_desc)

#define VIDXD_MAX_PORTALS 64

struct idxd_wq_desc_elem {
	enum idxd_portal_prot portal_prot;
	u8   portal_id;
	u8  work_desc[IDXD_DESC_SIZE];
	struct list_head link;
};

struct idxd_wq_portal {
	u8 data[IDXD_DESC_SIZE];
	unsigned int count;
};

struct idxd_virtual_wq {
	unsigned int ndescs;
	struct list_head head;
	struct idxd_wq_portal portals[VIDXD_MAX_PORTALS];
};

struct idxd_vdev {
	struct mdev_device *mdev;
	struct vfio_group *vfio_group;
	struct notifier_block pasid_nb;
	struct mutex ioasid_lock;
	struct list_head mm_list;
};

struct vdcm_idxd {
	struct vfio_device vdev;
	struct idxd_device *idxd;
	struct idxd_wq *wq;
	struct idxd_virtual_wq vwq;
	struct idxd_vdev ivdev;
	struct vdcm_idxd_type *type;
	int num_wqs;

	/* For VM use case */
	u64 bar_val[VIDXD_MAX_BARS];
	u64 bar_size[VIDXD_MAX_BARS];
	u8 cfg[VIDXD_MAX_CFG_SPACE_SZ];
	u8 bar0[VIDXD_MAX_MMIO_SPACE_SZ];
	struct list_head list;
	struct mutex dev_lock; /* lock for vidxd resources */
	struct mutex mig_submit_lock;
	bool paused;

	int refcount;
	struct vfio_pci_core_device vfio_pdev;
};

#define vdev_to_vidxd(vdev) container_of(vdev, struct vdcm_idxd, vdev)

static inline struct vdcm_idxd *to_vidxd(struct idxd_vdev *vdev)
{
	return container_of(vdev, struct vdcm_idxd, vdev);
}

#define IDXD_MDEV_NAME_LEN 64

enum idxd_mdev_type {
	IDXD_MDEV_TYPE_NONE = -1,
	IDXD_MDEV_TYPE_DSA_1_DWQ = 0,
	IDXD_MDEV_TYPE_IAX_1_DWQ,
	IDXD_MDEV_TYPE_DSA_1_SWQ,
	IDXD_MDEV_TYPE_IAX_1_SWQ,
};

#define IDXD_MDEV_WQ_TYPES 	2
#define IDXD_MDEV_TYPES		(IDXD_TYPE_MAX * IDXD_MDEV_WQ_TYPES)

struct vdcm_idxd_type {
	const char *name;
	enum idxd_mdev_type type;
	unsigned int avail_instance;
};

enum idxd_vdcm_rw {
	IDXD_VDCM_READ = 0,
	IDXD_VDCM_WRITE,
};

static inline u64 get_reg_val(void *buf, int size)
{
	u64 val = 0;

	switch (size) {
	case 8:
		val = *(u64 *)buf;
		break;
	case 4:
		val = *(u32 *)buf;
		break;
	case 2:
		val = *(u16 *)buf;
		break;
	case 1:
		val = *(u8 *)buf;
		break;
	}

	return val;
}

static inline u8 vidxd_state(struct vdcm_idxd *vidxd)
{
	union gensts_reg *gensts = (union gensts_reg *)(vidxd->bar0 + IDXD_GENSTATS_OFFSET);

	return gensts->state;
}

int idxd_mdev_host_init(struct idxd_device *idxd, const struct mdev_parent_ops *ops);
void idxd_mdev_host_release(struct kref *kref);
int idxd_mdev_get_pasid(struct mdev_device *mdev, u32 *pasid);
int idxd_mdev_get_host_pasid(struct mdev_device *mdev, u32 gpasid, u32 *pasid);
int vidxd_mmio_read(struct vdcm_idxd *vidxd, u64 pos, void *buf, unsigned int size);
int vidxd_mmio_write(struct vdcm_idxd *vidxd, u64 pos, void *buf, unsigned int size);
int vidxd_cfg_read(struct vdcm_idxd *vidxd, unsigned int pos, void *buf, unsigned int count);
int vidxd_cfg_write(struct vdcm_idxd *vidxd, unsigned int pos, void *buf, unsigned int size);
void vidxd_mmio_init(struct vdcm_idxd *vidxd);
void vidxd_reset(struct vdcm_idxd *vidxd, bool interrupt);
void vidxd_send_interrupt(struct vdcm_idxd *vidxd, int msix_idx);
void idxd_wq_vidxd_send_errors(struct idxd_wq *wq);

int vidxd_portal_mmio_read(struct vdcm_idxd *vidxd, u64 pos, void *buf,
				unsigned int size);
int vidxd_portal_mmio_write(struct vdcm_idxd *vidxd, u64 pos, void *buf,
				unsigned int size);

void vidxd_notify_revoked_handles (struct vdcm_idxd *vidxd);

#endif
