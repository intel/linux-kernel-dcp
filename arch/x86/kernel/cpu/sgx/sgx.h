/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _X86_SGX_H
#define _X86_SGX_H

#include <linux/bitops.h>
#include <linux/err.h>
#include <linux/io.h>
#include <linux/rwsem.h>
#include <linux/types.h>
#include <asm/asm.h>
#include <asm/sgx.h>

#undef pr_fmt
#define pr_fmt(fmt) "sgx: " fmt

#define EREMOVE_ERROR_MESSAGE \
	"EREMOVE returned %d (0x%x) and an EPC page was leaked. SGX may become unusable. " \
	"Refer to Documentation/x86/sgx.rst for more information."

#define SGX_MAX_EPC_SECTIONS		8
#define SGX_EEXTEND_BLOCK_SIZE		256
#define SGX_NR_TO_SCAN			16
#define SGX_NR_LOW_PAGES		32
#define SGX_NR_HIGH_PAGES		64

/* Pages, which are being tracked by the page reclaimer. */
#define SGX_EPC_PAGE_RECLAIMER_TRACKED	BIT(0)

/* VA page */
#define SGX_EPC_PAGE_VA			BIT(2)
/* Pages allocated for KVM guest */
#define SGX_EPC_PAGE_GUEST		BIT(3)
/*
 * Pages, failed to be zapped (EREMOVED)
 * by SGX CPUSVN update process.
 */
#define SGX_EPC_PAGE_ZAP_TRACKED	BIT(4)
/*
 * Pages, the associated enclave is being
 * released while SGX CPUSVN update is
 * running.
 */
#define SGX_EPC_PAGE_IN_RELEASE		BIT(5)

struct sgx_epc_page {
	unsigned int section;
	unsigned int flags;
	void *owner;
	struct list_head list;
};

/*
 * Contains the tracking data for NUMA nodes having EPC pages. Most importantly,
 * the free page list local to the node is stored here.
 */
struct sgx_numa_node {
	struct list_head free_page_list;
	spinlock_t lock;
};

/*
 * The firmware can define multiple chunks of EPC to the different areas of the
 * physical memory e.g. for memory areas of the each node. This structure is
 * used to store EPC pages for one EPC section and virtual memory area where
 * the pages have been mapped.
 */
struct sgx_epc_section {
	unsigned long phys_addr;
	void *virt_addr;
	struct sgx_epc_page *pages;
	struct sgx_numa_node *node;
	u64 size;
};

extern struct sgx_epc_section sgx_epc_sections[SGX_MAX_EPC_SECTIONS];

static inline unsigned long sgx_get_epc_phys_addr(struct sgx_epc_page *page)
{
	struct sgx_epc_section *section = &sgx_epc_sections[page->section];
	unsigned long index;

	index = ((unsigned long)page - (unsigned long)section->pages) / sizeof(*page);

	return section->phys_addr + index * PAGE_SIZE;
}

static inline void *sgx_get_epc_virt_addr(struct sgx_epc_page *page)
{
	struct sgx_epc_section *section = &sgx_epc_sections[page->section];
	unsigned long index;

	index = ((unsigned long)page - (unsigned long)section->pages) / sizeof(*page);

	return section->virt_addr + index * PAGE_SIZE;
}

struct sgx_epc_page *__sgx_alloc_epc_page(void);
void sgx_free_epc_page(struct sgx_epc_page *page);

void sgx_direct_reclaim(void);
void sgx_mark_page_reclaimable(struct sgx_epc_page *page);
int sgx_unmark_page_reclaimable(struct sgx_epc_page *page);
struct sgx_epc_page *sgx_alloc_epc_page(void *owner, bool reclaim);

void sgx_ipi_cb(void *info);

#ifdef CONFIG_X86_SGX_KVM
int __init sgx_vepc_init(void);
#else
static inline int __init sgx_vepc_init(void)
{
	return -ENODEV;
}
#endif

void sgx_update_lepubkeyhash(u64 *lepubkeyhash);

extern struct srcu_struct sgx_lock_epc_srcu;
bool sgx_epc_is_locked(void);
void sgx_zap_wakeup(void);
void sgx_zap_abort(void);

#endif /* _X86_SGX_H */
