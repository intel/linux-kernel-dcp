// SPDX-License-Identifier: GPL-2.0
/* CPU virtualization extensions handling */

#include <linux/gfp.h>
#include <linux/notifier.h>
#include <linux/percpu-defs.h>
#include <linux/reboot.h>
#include <linux/topology.h>

#include <asm/perf_event.h>
#include <asm/vmx.h>
#include <asm/virtext.h>

/* per-cpu VMCS pointer, holding VMCSs passed to VMXON instruction */
static DEFINE_PER_CPU(struct vmcs *, vmxon_vmcs);
/*
 * Cached to initialize VMCSs for VMXON instruction.
 *
 * VMCS size and revision id are supposed to be identical on all CPUs,
 * otherwise, migrating VMCSs between CPU is problematic. A warning is
 * emitted when VMXON is executed on a CPU that doesn't support a VMCS
 * of this revision id.
 */
static struct vmx_basic_info basic_info __read_mostly;

/* VMX's reference count on each CPU */
static DEFINE_PER_CPU(int, vmx_count);

/* Is system rebooting? */
static bool virt_rebooting;

/*
 * Handle a fault on a hardware virtualization (VMX or SVM) instruction.
 *
 * Hardware virtualization extension instructions may fault if a reboot turns
 * off virtualization while processes are running.  Usually after catching the
 * fault we just panic; during reboot instead the instruction is ignored.
 */
noinstr void virt_spurious_fault(void)
{
	/* Fault while not rebooting.  We want the trace. */
	BUG_ON(!virt_rebooting);
}
EXPORT_SYMBOL_GPL(virt_spurious_fault);

static int virt_reboot(struct notifier_block *notifier, unsigned long val,
		       void *v)
{
	virt_rebooting = true;
	return NOTIFY_OK;
}

static struct notifier_block virt_reboot_notifier = {
	.notifier_call = virt_reboot,
	.priority = 0,
};

noinline void vmptrld_error(struct vmcs *vmcs, u64 phys_addr)
{
	vmx_insn_failed("vmptrld failed: %p/%llx\n", vmcs, phys_addr);
}
EXPORT_SYMBOL_GPL(vmptrld_error);

int raw_vmcs_store(u64 *vmcs_pa)
{
	bool fault = 0;

	asm volatile("1: vmptrst %0\n\t"
		     "2:\n\t"
		     ".pushsection .fixup, \"ax\"\n\t"
		     "3: mov $1, %1\n\t"
		     "jmp 2b\n\t"
		     ".popsection\n\t"
		     _ASM_EXTABLE(1b, 3b)
		     : "=m" (*vmcs_pa), "+r" (fault) : :);

	if (fault) {
		virt_spurious_fault();
		return -EIO;
	}

	return 0;
}

static void free_vmxon_vmcs(int size)
{
	int cpu = raw_smp_processor_id();

	free_pages((unsigned long)per_cpu(vmxon_vmcs, cpu), get_order(size));
	per_cpu(vmxon_vmcs, cpu) = NULL;
}

static int alloc_vmxon_vmcs(int size, u32 rev_id)
{
	int node = cpu_to_node(raw_smp_processor_id());
	struct page *pages;
	struct vmcs *vmcs;

	pages = __alloc_pages_node(node, GFP_ATOMIC, get_order(size));
	if (!pages)
		return -ENOMEM;

	vmcs = page_address(pages);
	memset(vmcs, 0, size);
	vmcs->hdr.revision_id = rev_id;
	this_cpu_write(vmxon_vmcs, vmcs);

	return 0;
}

int cpu_vmx_get_basic_info(struct vmx_basic_info *info)
{
	u32 vmx_msr_low, vmx_msr_high;

	rdmsr(MSR_IA32_VMX_BASIC, vmx_msr_low, vmx_msr_high);

	/* IA-32 SDM Vol 3B: VMCS size is never greater than 4kB. */
	if ((vmx_msr_high & 0x1fff) > PAGE_SIZE)
		return -EIO;

#ifdef CONFIG_X86_64
	/* IA-32 SDM Vol 3B: 64-bit CPUs always have VMX_BASIC_MSR[48]==0. */
	if (vmx_msr_high & (1u<<16))
		return -EIO;
#endif

	/* Require Write-Back (WB) memory type for VMCS accesses. */
	if (((vmx_msr_high >> 18) & 15) != 6)
		return -EIO;

	info->size = vmx_msr_high & 0x1fff;
	info->cap = vmx_msr_high & ~0x1fff;
	info->rev_id = vmx_msr_low;

	return 0;
}
EXPORT_SYMBOL_GPL(cpu_vmx_get_basic_info);

static DEFINE_SPINLOCK(vmx_init_lock);

static int cpu_vmx_init(void)
{
	int ret = 0;
	u64 msr;

	/*
	 * cpu_vmx_init() is designed to be invoked once, subsequent
	 * invocations just return success. Hold a lock to prevent
	 * concurrent initializations.
	 */
	spin_lock(&vmx_init_lock);

	/* Check if this initialization is already done. */
	if (basic_info.size)
		goto unlock;

	if (!cpu_has_vmx()) {
		ret = -EOPNOTSUPP;
		goto unlock;
	}

	if (rdmsrl_safe(MSR_IA32_FEAT_CTL, &msr) ||
		!(msr & FEAT_CTL_LOCKED) ||
		!(msr & FEAT_CTL_VMX_ENABLED_OUTSIDE_SMX)) {
		ret = -EOPNOTSUPP;
		goto unlock;
	}

	ret = cpu_vmx_get_basic_info(&basic_info);
	if (ret)
		goto unlock;

	register_reboot_notifier(&virt_reboot_notifier);
unlock:
	spin_unlock(&vmx_init_lock);

	return ret;
}

/*
 * Increase VMX's reference count on current CPU. Preemption must be
 * disabled in advance.
 */
int cpu_vmx_get(void)
{
	int *count = this_cpu_ptr(&vmx_count);
	int r = 0;
	unsigned long flags;

	local_irq_save(flags);

	/* Retrieve VMX basic info if it isn't done */
	if (!basic_info.size) {
		r = cpu_vmx_init();
		if (r)
			goto restore;
	}

	if (*count == 0) {
		r = -EBUSY;
		/*
		 * someone else is manipulating (enable or disable) VMX.
		 * We cannot ensure VMX state as callers requested. Return
		 * an error to prevent unexpected behavior.
		 */
		if (cpu_vmx_enabled())
			goto restore;

		r = alloc_vmxon_vmcs(basic_info.size, basic_info.rev_id);
		if (r)
			goto restore;

		intel_pt_handle_vmx(1);
		r = cpu_vmxon(__pa(__this_cpu_read(vmxon_vmcs)));
		if (r) {
			intel_pt_handle_vmx(0);
			goto restore;
		}
	}
	*count += 1;

restore:
	local_irq_restore(flags);

	return r;
}
EXPORT_SYMBOL_GPL(cpu_vmx_get);

/*
 * Decrease VMX's reference count on current CPU. Preemption must be
 * disabled in advance.
 */
void cpu_vmx_put(void)
{
	int *count = this_cpu_ptr(&vmx_count);
	unsigned long flags;

	if (WARN_ON_ONCE(!count))
		return;

	local_irq_save(flags);

	if (*count == 1) {
		if (cpu_vmxoff())
			virt_spurious_fault();
		intel_pt_handle_vmx(0);
		free_vmxon_vmcs(basic_info.size);
	}
	*count -= 1;
	local_irq_restore(flags);
}
EXPORT_SYMBOL_GPL(cpu_vmx_put);
