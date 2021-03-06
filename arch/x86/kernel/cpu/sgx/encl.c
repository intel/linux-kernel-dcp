// SPDX-License-Identifier: GPL-2.0
/*  Copyright(c) 2016-20 Intel Corporation. */

#include <linux/lockdep.h>
#include <linux/mm.h>
#include <linux/mman.h>
#include <linux/shmem_fs.h>
#include <linux/suspend.h>
#include <linux/sched/mm.h>
#include <asm/sgx.h>
#include "encl.h"
#include "encls.h"
#include "sgx.h"

/*
 * ELDU: Load an EPC page as unblocked. For more info, see "OS Management of EPC
 * Pages" in the SDM.
 */
static int __sgx_encl_eldu(struct sgx_encl_page *encl_page,
			   struct sgx_epc_page *epc_page,
			   struct sgx_epc_page *secs_page)
{
	unsigned long va_offset = encl_page->desc & SGX_ENCL_PAGE_VA_OFFSET_MASK;
	struct sgx_encl *encl = encl_page->encl;
	struct sgx_pageinfo pginfo;
	struct sgx_backing b;
	pgoff_t page_index;
	int ret;

	if (secs_page)
		page_index = PFN_DOWN(encl_page->desc - encl_page->encl->base);
	else
		page_index = PFN_DOWN(encl->size);

	ret = sgx_encl_get_backing(encl, page_index, &b);
	if (ret)
		return ret;

	pginfo.addr = encl_page->desc & PAGE_MASK;
	pginfo.contents = (unsigned long)kmap_atomic(b.contents);
	pginfo.metadata = (unsigned long)kmap_atomic(b.pcmd) +
			  b.pcmd_offset;

	if (secs_page)
		pginfo.secs = (u64)sgx_get_epc_virt_addr(secs_page);
	else
		pginfo.secs = 0;

	ret = __eldu(&pginfo, sgx_get_epc_virt_addr(epc_page),
		     sgx_get_epc_virt_addr(encl_page->va_page->epc_page) + va_offset);
	if (ret) {
		if (encls_failed(ret))
			ENCLS_WARN(ret, "ELDU");

		ret = -EFAULT;
	}

	kunmap_atomic((void *)(unsigned long)(pginfo.metadata - b.pcmd_offset));
	kunmap_atomic((void *)(unsigned long)pginfo.contents);

	sgx_encl_put_backing(&b, false);

	return ret;
}

static struct sgx_epc_page *sgx_encl_eldu(struct sgx_encl_page *encl_page,
					  struct sgx_epc_page *secs_page)
{

	unsigned long va_offset = encl_page->desc & SGX_ENCL_PAGE_VA_OFFSET_MASK;
	struct sgx_encl *encl = encl_page->encl;
	struct sgx_epc_page *epc_page;
	int ret;

	epc_page = sgx_alloc_epc_page(encl_page, false);
	if (IS_ERR(epc_page))
		return epc_page;

	ret = __sgx_encl_eldu(encl_page, epc_page, secs_page);
	if (ret) {
		sgx_encl_free_epc_page(epc_page);
		return ERR_PTR(ret);
	}

	sgx_free_va_slot(encl_page->va_page, va_offset);
	list_move(&encl_page->va_page->list, &encl->va_pages);
	encl_page->desc &= ~SGX_ENCL_PAGE_VA_OFFSET_MASK;
	encl_page->epc_page = epc_page;

	return epc_page;
}

struct sgx_encl_page *sgx_encl_load_page(struct sgx_encl *encl,
					 unsigned long addr)
{
	struct sgx_epc_page *epc_page;
	struct sgx_encl_page *entry;

	entry = xa_load(&encl->page_array, PFN_DOWN(addr));
	if (!entry)
		return ERR_PTR(-EFAULT);

	/* Entry successfully located. */
	if (entry->epc_page) {
		if (entry->desc & SGX_ENCL_PAGE_BEING_RECLAIMED)
			return ERR_PTR(-EBUSY);

		return entry;
	}

	if (!(encl->secs.epc_page)) {
		epc_page = sgx_encl_eldu(&encl->secs, NULL);
		if (IS_ERR(epc_page))
			return ERR_CAST(epc_page);
	}

	epc_page = sgx_encl_eldu(entry, encl->secs.epc_page);
	if (IS_ERR(epc_page))
		return ERR_CAST(epc_page);

	encl->secs_child_cnt++;
	sgx_mark_page_reclaimable(entry->epc_page);

	return entry;
}

/**
 * sgx_encl_eaug_page - Dynamically add page to initialized enclave
 * @vma:	VMA obtained from fault info from where page is accessed
 * @encl:	enclave accessing the page
 * @addr:	address that triggered the page fault
 *
 * When an initialized enclave accesses a page with no backing EPC page
 * on a SGX2 system then the EPC can be added dynamically via the SGX2
 * ENCLS[EAUG] instruction.
 *
 * Returns: Appropriate vm_fault_t: VM_FAULT_NOPAGE when PTE was installed
 * successfully, VM_FAULT_SIGBUS or VM_FAULT_OOM as error otherwise.
 */
static vm_fault_t sgx_encl_eaug_page(struct vm_area_struct *vma,
				     struct sgx_encl *encl, unsigned long addr)
{
	struct sgx_pageinfo pginfo = {0};
	struct sgx_encl_page *encl_page;
	struct sgx_epc_page *epc_page;
	struct sgx_va_page *va_page;
	unsigned long phys_addr;
	unsigned long prot;
	vm_fault_t vmret;
	int ret;

	if (!test_bit(SGX_ENCL_INITIALIZED, &encl->flags))
		return VM_FAULT_SIGBUS;

	encl_page = kzalloc(sizeof(*encl_page), GFP_KERNEL);
	if (!encl_page)
		return VM_FAULT_OOM;

	encl_page->desc = addr;
	encl_page->encl = encl;

	/*
	 * Adding a regular page that is architecturally allowed to only
	 * be created with RW permissions.
	 * TBD: Interface with user space policy to support max permissions
	 * of RWX.
	 */
	prot = PROT_READ | PROT_WRITE;
	encl_page->vm_run_prot_bits = calc_vm_prot_bits(prot, 0);
	encl_page->vm_max_prot_bits = encl_page->vm_run_prot_bits;

	epc_page = sgx_alloc_epc_page(encl_page, true);
	if (IS_ERR(epc_page)) {
		kfree(encl_page);
		return VM_FAULT_SIGBUS;
	}

	va_page = sgx_encl_grow(encl);
	if (IS_ERR(va_page)) {
		ret = PTR_ERR(va_page);
		goto err_out_free;
	}

	mutex_lock(&encl->lock);

	/*
	 * Copy comment from sgx_encl_add_page() to maintain guidance in
	 * this similar flow:
	 * Adding to encl->va_pages must be done under encl->lock.  Ditto for
	 * deleting (via sgx_encl_shrink()) in the error path.
	 */
	if (va_page)
		list_add(&va_page->list, &encl->va_pages);

	ret = xa_insert(&encl->page_array, PFN_DOWN(encl_page->desc),
			encl_page, GFP_KERNEL);
	/*
	 * If ret == -EBUSY then page was created in another flow while
	 * running without encl->lock
	 */
	if (ret)
		goto err_out_unlock;

	pginfo.secs = (unsigned long)sgx_get_epc_virt_addr(encl->secs.epc_page);
	pginfo.addr = encl_page->desc & PAGE_MASK;
	pginfo.metadata = 0;

	ret = __eaug(&pginfo, sgx_get_epc_virt_addr(epc_page));
	if (ret)
		goto err_out;

	encl_page->encl = encl;
	encl_page->epc_page = epc_page;
	encl_page->type = SGX_PAGE_TYPE_REG;
	encl->secs_child_cnt++;

	sgx_mark_page_reclaimable(encl_page->epc_page);

	phys_addr = sgx_get_epc_phys_addr(epc_page);
	/*
	 * Do not undo everything when creating PTE entry fails - next #PF
	 * would find page ready for a PTE.
	 * PAGE_SHARED because protection is forced to be RW above and COW
	 * is not supported.
	 */
	vmret = vmf_insert_pfn_prot(vma, addr, PFN_DOWN(phys_addr),
				    PAGE_SHARED);
	if (vmret != VM_FAULT_NOPAGE) {
		mutex_unlock(&encl->lock);
		return VM_FAULT_SIGBUS;
	}
	mutex_unlock(&encl->lock);
	return VM_FAULT_NOPAGE;

err_out:
	xa_erase(&encl->page_array, PFN_DOWN(encl_page->desc));

err_out_unlock:
	sgx_encl_shrink(encl, va_page);
	mutex_unlock(&encl->lock);

err_out_free:
	sgx_encl_free_epc_page(epc_page);
	kfree(encl_page);

	return VM_FAULT_SIGBUS;
}

static vm_fault_t sgx_vma_fault(struct vm_fault *vmf)
{
	unsigned long addr = (unsigned long)vmf->address;
	struct vm_area_struct *vma = vmf->vma;
	unsigned long page_prot_bits;
	struct sgx_encl_page *entry;
	unsigned long vm_prot_bits;
	unsigned long phys_addr;
	struct sgx_encl *encl;
	vm_fault_t ret;
	int srcu_idx;

	encl = vma->vm_private_data;

	/*
	 * It's very unlikely but possible that allocating memory for the
	 * mm_list entry of a forked process failed in sgx_vma_open(). When
	 * this happens, vm_private_data is set to NULL.
	 */
	if (unlikely(!encl))
		return VM_FAULT_SIGBUS;

	srcu_idx = srcu_read_lock(&sgx_lock_epc_srcu);
	if (sgx_epc_is_locked()) {
		srcu_read_unlock(&sgx_lock_epc_srcu, srcu_idx);
		return VM_FAULT_SIGBUS;
	}

	/*
	 * The page_array keeps track of all enclave pages, whether they
	 * are swapped out or not. If there is no entry for this page and
	 * the system supports SGX2 then it is possible to dynamically add
	 * a new enclave page. This is only possible for an initialized
	 * enclave that will be checked for right away.
	 */
	if (cpu_feature_enabled(X86_FEATURE_SGX2) &&
	    (!xa_load(&encl->page_array, PFN_DOWN(addr)))) {
		ret = sgx_encl_eaug_page(vma, encl, addr);
		srcu_read_unlock(&sgx_lock_epc_srcu, srcu_idx);
		return ret;
	}

	mutex_lock(&encl->lock);

	entry = sgx_encl_load_page(encl, addr);
	if (IS_ERR(entry)) {
		mutex_unlock(&encl->lock);
		srcu_read_unlock(&sgx_lock_epc_srcu, srcu_idx);

		if (PTR_ERR(entry) == -EBUSY)
			return VM_FAULT_NOPAGE;

		return VM_FAULT_SIGBUS;
	}

	phys_addr = sgx_get_epc_phys_addr(entry->epc_page);

	/*
	 * Insert PTE to match the EPCM page permissions ensured to not
	 * exceed the VMA permissions.
	 */
	vm_prot_bits = vma->vm_flags & (VM_READ | VM_WRITE | VM_EXEC);
	page_prot_bits = entry->vm_run_prot_bits & vm_prot_bits;
	/*
	 * Add VM_SHARED so that PTE is made writable right away if VMA
	 * and EPCM are writable (no COW in SGX).
	 */
	page_prot_bits |= (vma->vm_flags & VM_SHARED);
	ret = vmf_insert_pfn_prot(vma, addr, PFN_DOWN(phys_addr),
				  vm_get_page_prot(page_prot_bits));
	if (ret != VM_FAULT_NOPAGE) {
		mutex_unlock(&encl->lock);
		srcu_read_unlock(&sgx_lock_epc_srcu, srcu_idx);

		return VM_FAULT_SIGBUS;
	}

	sgx_encl_test_and_clear_young(vma->vm_mm, entry);
	mutex_unlock(&encl->lock);
	srcu_read_unlock(&sgx_lock_epc_srcu, srcu_idx);

	return VM_FAULT_NOPAGE;
}

/*
 * A fault occurred while writing to a present enclave PTE. Since PTE is
 * present this will not be handled by sgx_vma_fault(). VMA may allow
 * writing to the page while enclave does not. Do not follow the default
 * of inheriting VMA permissions in this regard, ensure enclave also allows
 * writing to the page.
 */
static vm_fault_t sgx_vma_pfn_mkwrite(struct vm_fault *vmf)
{
	unsigned long addr = (unsigned long)vmf->address;
	struct vm_area_struct *vma = vmf->vma;
	struct sgx_encl_page *entry;
	struct sgx_encl *encl;
	vm_fault_t ret = 0;

	encl = vma->vm_private_data;

	/*
	 * It's very unlikely but possible that allocating memory for the
	 * mm_list entry of a forked process failed in sgx_vma_open(). When
	 * this happens, vm_private_data is set to NULL.
	 */
	if (unlikely(!encl))
		return VM_FAULT_SIGBUS;

	mutex_lock(&encl->lock);

	entry = xa_load(&encl->page_array, PFN_DOWN(addr));
	if (!entry) {
		ret = VM_FAULT_SIGBUS;
		goto out;
	}

	if (!(entry->vm_run_prot_bits & VM_WRITE))
		ret = VM_FAULT_SIGBUS;

out:
	mutex_unlock(&encl->lock);
	return ret;
}

static void sgx_vma_open(struct vm_area_struct *vma)
{
	struct sgx_encl *encl = vma->vm_private_data;

	/*
	 * It's possible but unlikely that vm_private_data is NULL. This can
	 * happen in a grandchild of a process, when sgx_encl_mm_add() had
	 * failed to allocate memory in this callback.
	 */
	if (unlikely(!encl))
		return;

	if (sgx_encl_mm_add(encl, vma->vm_mm))
		vma->vm_private_data = NULL;
}


/**
 * sgx_encl_may_map() - Check if a requested VMA mapping is allowed
 * @encl:		an enclave pointer
 * @start:		lower bound of the address range, inclusive
 * @end:		upper bound of the address range, exclusive
 * @vm_flags:		VMA flags
 *
 * Iterate through the enclave pages contained within [@start, @end) to verify
 * that the permissions requested by a subset of {VM_READ, VM_WRITE, VM_EXEC}
 * do not contain any permissions that are not contained in the build time
 * permissions of any of the enclave pages within the given address range.
 *
 * An enclave creator must declare the strongest permissions that will be
 * needed for each enclave page. This ensures that mappings have the identical
 * or weaker permissions than the earlier declared permissions.
 *
 * Return: 0 on success, -EACCES otherwise
 */
int sgx_encl_may_map(struct sgx_encl *encl, unsigned long start,
		     unsigned long end, unsigned long vm_flags)
{
	unsigned long vm_prot_bits = vm_flags & (VM_READ | VM_WRITE | VM_EXEC);
	struct sgx_encl_page *page;
	unsigned long count = 0;
	int ret = 0;

	XA_STATE(xas, &encl->page_array, PFN_DOWN(start));

	if (test_bit(SGX_ENCL_INITIALIZED, &encl->flags) &&
	    (start < encl->base || end > encl->base + encl->size))
		return -EACCES;

	/*
	 * Disallow READ_IMPLIES_EXEC tasks as their VMA permissions might
	 * conflict with the enclave page permissions.
	 */
	if (current->personality & READ_IMPLIES_EXEC)
		return -EACCES;

	mutex_lock(&encl->lock);
	xas_lock(&xas);
	xas_for_each(&xas, page, PFN_DOWN(end - 1)) {
		if (~page->vm_run_prot_bits & vm_prot_bits) {
			ret = -EACCES;
			break;
		}

		/* Reschedule on every XA_CHECK_SCHED iteration. */
		if (!(++count % XA_CHECK_SCHED)) {
			xas_pause(&xas);
			xas_unlock(&xas);
			mutex_unlock(&encl->lock);

			cond_resched();

			mutex_lock(&encl->lock);
			xas_lock(&xas);
		}
	}
	xas_unlock(&xas);
	mutex_unlock(&encl->lock);

	return ret;
}

static int sgx_vma_mprotect(struct vm_area_struct *vma, unsigned long start,
			    unsigned long end, unsigned long newflags)
{
	return sgx_encl_may_map(vma->vm_private_data, start, end, newflags);
}

static int sgx_encl_debug_read(struct sgx_encl *encl, struct sgx_encl_page *page,
			       unsigned long addr, void *data)
{
	unsigned long offset = addr & ~PAGE_MASK;
	int ret;


	ret = __edbgrd(sgx_get_epc_virt_addr(page->epc_page) + offset, data);
	if (ret)
		return -EIO;

	return 0;
}

static int sgx_encl_debug_write(struct sgx_encl *encl, struct sgx_encl_page *page,
				unsigned long addr, void *data)
{
	unsigned long offset = addr & ~PAGE_MASK;
	int ret;

	ret = __edbgwr(sgx_get_epc_virt_addr(page->epc_page) + offset, data);
	if (ret)
		return -EIO;

	return 0;
}

/*
 * Load an enclave page to EPC if required, and take encl->lock.
 */
static struct sgx_encl_page *sgx_encl_reserve_page(struct sgx_encl *encl,
						   unsigned long addr)
{
	struct sgx_encl_page *entry;

	for ( ; ; ) {
		mutex_lock(&encl->lock);

		entry = sgx_encl_load_page(encl, addr);
		if (PTR_ERR(entry) != -EBUSY)
			break;

		mutex_unlock(&encl->lock);
	}

	if (IS_ERR(entry))
		mutex_unlock(&encl->lock);

	return entry;
}

static int sgx_vma_access(struct vm_area_struct *vma, unsigned long addr,
			  void *buf, int len, int write)
{
	struct sgx_encl *encl = vma->vm_private_data;
	struct sgx_encl_page *entry = NULL;
	char data[sizeof(unsigned long)];
	unsigned long align;
	int offset;
	int cnt;
	int ret = 0;
	int i;
	int srcu_idx;

	/*
	 * If process was forked, VMA is still there but vm_private_data is set
	 * to NULL.
	 */
	if (!encl)
		return -EFAULT;

	if (!test_bit(SGX_ENCL_DEBUG, &encl->flags))
		return -EFAULT;

	for (i = 0; i < len; i += cnt) {
		srcu_idx = srcu_read_lock(&sgx_lock_epc_srcu);
		if (sgx_epc_is_locked()) {
			ret = -EBUSY;
			goto out;
		}

		entry = sgx_encl_reserve_page(encl, (addr + i) & PAGE_MASK);
		if (IS_ERR(entry)) {
			ret = PTR_ERR(entry);
			break;
		}

		align = ALIGN_DOWN(addr + i, sizeof(unsigned long));
		offset = (addr + i) & (sizeof(unsigned long) - 1);
		cnt = sizeof(unsigned long) - offset;
		cnt = min(cnt, len - i);

		ret = sgx_encl_debug_read(encl, entry, align, data);
		if (ret)
			goto out;

		if (write) {
			memcpy(data + offset, buf + i, cnt);
			ret = sgx_encl_debug_write(encl, entry, align, data);
			if (ret)
				goto out;
		} else {
			memcpy(buf + i, data + offset, cnt);
		}

out:
		mutex_unlock(&encl->lock);
		srcu_read_unlock(&sgx_lock_epc_srcu, srcu_idx);

		if (ret)
			break;
	}

	return ret < 0 ? ret : i;
}

const struct vm_operations_struct sgx_vm_ops = {
	.fault = sgx_vma_fault,
	.mprotect = sgx_vma_mprotect,
	.open = sgx_vma_open,
	.access = sgx_vma_access,
	.pfn_mkwrite = sgx_vma_pfn_mkwrite,
};

/**
 * sgx_encl_release - Destroy an enclave instance
 * @ref:	address of a kref inside &sgx_encl
 *
 * Used together with kref_put(). Frees all the resources associated with the
 * enclave and the instance itself.
 */
void sgx_encl_release(struct kref *ref)
{
	struct sgx_encl *encl = container_of(ref, struct sgx_encl, refcount);
	struct sgx_va_page *va_page;
	struct sgx_encl_page *entry;
	unsigned long index;

	xa_for_each(&encl->page_array, index, entry) {
		if (entry->epc_page) {
			/*
			 * The page and its radix tree entry cannot be freed
			 * if the page is being held by the reclaimer.
			 */
			if (sgx_unmark_page_reclaimable(entry->epc_page))
				continue;

			sgx_encl_free_epc_page(entry->epc_page);
			encl->secs_child_cnt--;
			entry->epc_page = NULL;
		}

		kfree(entry);
		/* Invoke scheduler to prevent soft lockups. */
		cond_resched();
	}

	xa_destroy(&encl->page_array);

	if (!encl->secs_child_cnt && encl->secs.epc_page) {
		sgx_encl_free_epc_page(encl->secs.epc_page);
		encl->secs.epc_page = NULL;
	}

	while (!list_empty(&encl->va_pages)) {
		va_page = list_first_entry(&encl->va_pages, struct sgx_va_page,
					   list);
		list_del(&va_page->list);
		sgx_encl_free_epc_page(va_page->epc_page);
		kfree(va_page);
	}

	if (encl->backing)
		fput(encl->backing);

	cleanup_srcu_struct(&encl->srcu);

	WARN_ON_ONCE(!list_empty(&encl->mm_list));

	/* Detect EPC page leak's. */
	WARN_ON_ONCE(encl->secs_child_cnt);
	WARN_ON_ONCE(encl->secs.epc_page);

	/*
	 * EPC pages were freed and EREMOVE was executed. Wake
	 * up any zappers which were waiting for this.
	 */
	sgx_zap_wakeup();

	kfree(encl);
}

/*
 * 'mm' is exiting and no longer needs mmu notifications.
 */
static void sgx_mmu_notifier_release(struct mmu_notifier *mn,
				     struct mm_struct *mm)
{
	struct sgx_encl_mm *encl_mm = container_of(mn, struct sgx_encl_mm, mmu_notifier);
	struct sgx_encl_mm *tmp = NULL;

	/*
	 * The enclave itself can remove encl_mm.  Note, objects can't be moved
	 * off an RCU protected list, but deletion is ok.
	 */
	spin_lock(&encl_mm->encl->mm_lock);
	list_for_each_entry(tmp, &encl_mm->encl->mm_list, list) {
		if (tmp == encl_mm) {
			list_del_rcu(&encl_mm->list);
			break;
		}
	}
	spin_unlock(&encl_mm->encl->mm_lock);

	if (tmp == encl_mm) {
		synchronize_srcu(&encl_mm->encl->srcu);
		mmu_notifier_put(mn);
	}
}

static void sgx_mmu_notifier_free(struct mmu_notifier *mn)
{
	struct sgx_encl_mm *encl_mm = container_of(mn, struct sgx_encl_mm, mmu_notifier);

	/* 'encl_mm' is going away, put encl_mm->encl reference: */
	kref_put(&encl_mm->encl->refcount, sgx_encl_release);

	kfree(encl_mm);
}

static const struct mmu_notifier_ops sgx_mmu_notifier_ops = {
	.release		= sgx_mmu_notifier_release,
	.free_notifier		= sgx_mmu_notifier_free,
};

static struct sgx_encl_mm *sgx_encl_find_mm(struct sgx_encl *encl,
					    struct mm_struct *mm)
{
	struct sgx_encl_mm *encl_mm = NULL;
	struct sgx_encl_mm *tmp;
	int idx;

	idx = srcu_read_lock(&encl->srcu);

	list_for_each_entry_rcu(tmp, &encl->mm_list, list) {
		if (tmp->mm == mm) {
			encl_mm = tmp;
			break;
		}
	}

	srcu_read_unlock(&encl->srcu, idx);

	return encl_mm;
}

int sgx_encl_mm_add(struct sgx_encl *encl, struct mm_struct *mm)
{
	struct sgx_encl_mm *encl_mm;
	int ret;

	/*
	 * Even though a single enclave may be mapped into an mm more than once,
	 * each 'mm' only appears once on encl->mm_list. This is guaranteed by
	 * holding the mm's mmap lock for write before an mm can be added or
	 * remove to an encl->mm_list.
	 */
	mmap_assert_write_locked(mm);

	/*
	 * It's possible that an entry already exists in the mm_list, because it
	 * is removed only on VFS release or process exit.
	 */
	if (sgx_encl_find_mm(encl, mm))
		return 0;

	encl_mm = kzalloc(sizeof(*encl_mm), GFP_KERNEL);
	if (!encl_mm)
		return -ENOMEM;

	/* Grab a refcount for the encl_mm->encl reference: */
	kref_get(&encl->refcount);
	encl_mm->encl = encl;
	encl_mm->mm = mm;
	encl_mm->mmu_notifier.ops = &sgx_mmu_notifier_ops;

	ret = __mmu_notifier_register(&encl_mm->mmu_notifier, mm);
	if (ret) {
		kfree(encl_mm);
		return ret;
	}

	spin_lock(&encl->mm_lock);
	list_add_rcu(&encl_mm->list, &encl->mm_list);
	/* Pairs with smp_rmb() in sgx_zap_enclave_ptes(). */
	smp_wmb();
	encl->mm_list_version++;
	spin_unlock(&encl->mm_lock);

	return 0;
}

/**
 * sgx_encl_cpumask - Query which CPUs might be accessing the enclave
 * @encl: the enclave
 *
 * Some SGX functions require that no cached linear-to-physical address
 * mappings are present before they can succeed. For example, ENCLS[EWB]
 * copies a page from the enclave page cache to regular main memory but
 * it fails if it cannot ensure that there are no cached
 * linear-to-physical address mappings referring to the page.
 *
 * SGX hardware flushes all cached linear-to-physical mappings on a CPU
 * when an enclave is exited via ENCLU[EEXIT] or an Asynchronous Enclave
 * Exit (AEX). Exiting an enclave will thus ensure cached linear-to-physical
 * address mappings are cleared but coordination with the tracking done within
 * the SGX hardware is needed to support the SGX functions that depend on this
 * cache clearing.
 *
 * When the ENCLS[ETRACK] function is issued on an enclave the hardware
 * tracks threads operating inside the enclave at that time. The SGX
 * hardware tracking require that all the identified threads must have
 * exited the enclave in order to flush the mappings before a function such
 * as ENCLS[EWB] will be permitted
 *
 * The following flow is used to support SGX functions that require that
 * no cached linear-to-physical address mappings are present:
 * 1) Execute ENCLS[ETRACK] to initiate hardware tracking.
 * 2) Use this function (sgx_encl_cpumask()) to query which CPUs might be
 *    accessing the enclave.
 * 3) Send IPI to identified CPUs, kicking them out of the enclave and
 *    thus flushing all locally cached linear-to-physical address mappings.
 * 4) Execute SGX function.
 *
 * Context: It is required to call this function after ENCLS[ETRACK].
 *          This will ensure that if any new mm appears (racing with
 *          sgx_encl_mm_add()) then the new mm will enter into the
 *          enclave with fresh linear-to-physical address mappings.
 *
 *          It is required that all IPIs are completed before a new
 *          ENCLS[ETRACK] is issued so be sure to protect steps 1 to 3
 *          of the above flow with the enclave's mutex.
 *
 * Return: cpumask of CPUs that might be accessing @encl
 */
const cpumask_t *sgx_encl_cpumask(struct sgx_encl *encl)
{
	cpumask_t *cpumask = &encl->cpumask;
	struct sgx_encl_mm *encl_mm;
	int idx;

	cpumask_clear(cpumask);

	idx = srcu_read_lock(&encl->srcu);

	list_for_each_entry_rcu(encl_mm, &encl->mm_list, list) {
		if (!mmget_not_zero(encl_mm->mm))
			continue;

		cpumask_or(cpumask, cpumask, mm_cpumask(encl_mm->mm));

		mmput_async(encl_mm->mm);
	}

	srcu_read_unlock(&encl->srcu, idx);

	return cpumask;
}

static struct page *sgx_encl_get_backing_page(struct sgx_encl *encl,
					      pgoff_t index)
{
	struct inode *inode = encl->backing->f_path.dentry->d_inode;
	struct address_space *mapping = inode->i_mapping;
	gfp_t gfpmask = mapping_gfp_mask(mapping);

	return shmem_read_mapping_page_gfp(mapping, index, gfpmask);
}

/**
 * sgx_encl_get_backing() - Pin the backing storage
 * @encl:	an enclave pointer
 * @page_index:	enclave page index
 * @backing:	data for accessing backing storage for the page
 *
 * Pin the backing storage pages for storing the encrypted contents and Paging
 * Crypto MetaData (PCMD) of an enclave page.
 *
 * Return:
 *   0 on success,
 *   -errno otherwise.
 */
int sgx_encl_get_backing(struct sgx_encl *encl, unsigned long page_index,
			 struct sgx_backing *backing)
{
	pgoff_t pcmd_index = PFN_DOWN(encl->size) + 1 + (page_index >> 5);
	struct page *contents;
	struct page *pcmd;

	contents = sgx_encl_get_backing_page(encl, page_index);
	if (IS_ERR(contents))
		return PTR_ERR(contents);

	pcmd = sgx_encl_get_backing_page(encl, pcmd_index);
	if (IS_ERR(pcmd)) {
		put_page(contents);
		return PTR_ERR(pcmd);
	}

	backing->page_index = page_index;
	backing->contents = contents;
	backing->pcmd = pcmd;
	backing->pcmd_offset =
		(page_index & (PAGE_SIZE / sizeof(struct sgx_pcmd) - 1)) *
		sizeof(struct sgx_pcmd);

	return 0;
}

/**
 * sgx_encl_put_backing() - Unpin the backing storage
 * @backing:	data for accessing backing storage for the page
 * @do_write:	mark pages dirty
 */
void sgx_encl_put_backing(struct sgx_backing *backing, bool do_write)
{
	if (do_write) {
		set_page_dirty(backing->pcmd);
		set_page_dirty(backing->contents);
	}

	put_page(backing->pcmd);
	put_page(backing->contents);
}

static int sgx_encl_test_and_clear_young_cb(pte_t *ptep, unsigned long addr,
					    void *data)
{
	pte_t pte;
	int ret;

	ret = pte_young(*ptep);
	if (ret) {
		pte = pte_mkold(*ptep);
		set_pte_at((struct mm_struct *)data, addr, ptep, pte);
	}

	return ret;
}

/**
 * sgx_encl_test_and_clear_young() - Test and reset the accessed bit
 * @mm:		mm_struct that is checked
 * @page:	enclave page to be tested for recent access
 *
 * Checks the Access (A) bit from the PTE corresponding to the enclave page and
 * clears it.
 *
 * Return: 1 if the page has been recently accessed and 0 if not.
 */
int sgx_encl_test_and_clear_young(struct mm_struct *mm,
				  struct sgx_encl_page *page)
{
	unsigned long addr = page->desc & PAGE_MASK;
	struct sgx_encl *encl = page->encl;
	struct vm_area_struct *vma;
	int ret;

	ret = sgx_encl_find(mm, addr, &vma);
	if (ret)
		return 0;

	if (encl != vma->vm_private_data)
		return 0;

	ret = apply_to_page_range(vma->vm_mm, addr, PAGE_SIZE,
				  sgx_encl_test_and_clear_young_cb, vma->vm_mm);
	if (ret < 0)
		return 0;

	return ret;
}

/**
 * sgx_zap_enclave_ptes - remove PTEs mapping the address from enclave
 * @encl: the enclave
 * @addr: page aligned pointer to single page for which PTEs will be removed
 *
 * Multiple VMAs may have an enclave page mapped. Remove the PTE mapping
 * @addr from each VMA. Ensure that page fault handler is ready to handle
 * new mappings of @addr before calling this function.
 */
void sgx_zap_enclave_ptes(struct sgx_encl *encl, unsigned long addr)
{
	unsigned long mm_list_version;
	struct sgx_encl_mm *encl_mm;
	struct vm_area_struct *vma;
	int idx, ret;

	do {
		mm_list_version = encl->mm_list_version;

		/* Pairs with smp_wmb() in sgx_encl_mm_add(). */
		smp_rmb();

		idx = srcu_read_lock(&encl->srcu);

		list_for_each_entry_rcu(encl_mm, &encl->mm_list, list) {
			if (!mmget_not_zero(encl_mm->mm))
				continue;

			mmap_read_lock(encl_mm->mm);

			ret = sgx_encl_find(encl_mm->mm, addr, &vma);
			if (!ret && encl == vma->vm_private_data)
				zap_vma_ptes(vma, addr, PAGE_SIZE);

			mmap_read_unlock(encl_mm->mm);

			mmput_async(encl_mm->mm);
		}

		srcu_read_unlock(&encl->srcu, idx);
	} while (unlikely(encl->mm_list_version != mm_list_version));
}

/**
 * sgx_alloc_va_page() - Allocate a Version Array (VA) page
 * @va_page:	struct sgx_va_page connected to this VA page
 *
 * Allocate a free EPC page and convert it to a Version Array (VA) page.
 *
 * Return:
 *   a VA page,
 *   -errno otherwise
 */
struct sgx_epc_page *sgx_alloc_va_page(struct sgx_va_page *va_page)
{
	struct sgx_epc_page *epc_page;
	int ret;

	epc_page = sgx_alloc_epc_page(va_page, true);
	if (IS_ERR(epc_page))
		return ERR_CAST(epc_page);

	ret = __epa(sgx_get_epc_virt_addr(epc_page));
	if (ret) {
		WARN_ONCE(1, "EPA returned %d (0x%x)", ret, ret);
		sgx_encl_free_epc_page(epc_page);
		return ERR_PTR(-EFAULT);
	}

	epc_page->flags |= SGX_EPC_PAGE_VA;

	return epc_page;
}

/**
 * sgx_alloc_va_slot - allocate a VA slot
 * @va_page:	a &struct sgx_va_page instance
 *
 * Allocates a slot from a &struct sgx_va_page instance.
 *
 * Return: offset of the slot inside the VA page
 */
unsigned int sgx_alloc_va_slot(struct sgx_va_page *va_page)
{
	int slot = find_first_zero_bit(va_page->slots, SGX_VA_SLOT_COUNT);

	if (slot < SGX_VA_SLOT_COUNT)
		set_bit(slot, va_page->slots);

	return slot << 3;
}

/**
 * sgx_free_va_slot - free a VA slot
 * @va_page:	a &struct sgx_va_page instance
 * @offset:	offset of the slot inside the VA page
 *
 * Frees a slot from a &struct sgx_va_page instance.
 */
void sgx_free_va_slot(struct sgx_va_page *va_page, unsigned int offset)
{
	clear_bit(offset >> 3, va_page->slots);
}

/**
 * sgx_va_page_full - is the VA page full?
 * @va_page:	a &struct sgx_va_page instance
 *
 * Return: true if all slots have been taken
 */
bool sgx_va_page_full(struct sgx_va_page *va_page)
{
	int slot = find_first_zero_bit(va_page->slots, SGX_VA_SLOT_COUNT);

	return slot == SGX_VA_SLOT_COUNT;
}

/**
 * sgx_encl_free_epc_page - free an EPC page assigned to an enclave
 * @page:	EPC page to be freed
 *
 * Free an EPC page assigned to an enclave. It does EREMOVE for the page, and
 * only upon success, it puts the page back to free page list.  Otherwise, it
 * gives a WARNING to indicate page is leaked.
 */
void sgx_encl_free_epc_page(struct sgx_epc_page *page)
{
	int ret;

	WARN_ON_ONCE(page->flags & SGX_EPC_PAGE_RECLAIMER_TRACKED);

	ret = __eremove(sgx_get_epc_virt_addr(page));
	if (WARN_ONCE(ret, EREMOVE_ERROR_MESSAGE, ret, ret)) {
		/*
		 * The EREMOVE failed. If a CPUSVN is in progress,
		 * it is now expected to fail. Notify it.
		 */
		sgx_zap_abort();
		return;
	}

	sgx_free_epc_page(page);
}
