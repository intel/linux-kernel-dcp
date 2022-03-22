// SPDX-License-Identifier: GPL-2.0
/* Load and initialize TDX-module. */

#define pr_fmt(fmt) "tdx: " fmt

#include <linux/earlycpio.h>
#include <linux/memblock.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/slab.h>
#include <linux/cpu.h>
#include <linux/platform_device.h>
#include <linux/firmware.h>

#include <asm/irq_vectors.h>
#include <asm/trace/seam.h>
#include <asm/tdx_errno.h>
#include <asm/tdx_arch.h>
#include <asm/tdx_host.h>
#include <asm/virtext.h>
#include <asm/apic.h>
#include <asm/nmi.h>

#include "tdmr-sysmem.h"
#include "tdmr-legacy-pmem.h"
#include "tdx-tdmr.h"
#include "seamcall.h"
#include "tdx-ops.h"
#include "p-seamldr.h"
#include "seam.h"
#include "tdx.h"

enum TDX_HOST_OPTION {
	TDX_HOST_OFF,
	TDX_HOST_ON,
};

/* Fake device for request_firmware_direct */
static struct platform_device *tdx_module_dev;

static enum TDX_HOST_OPTION tdx_host __initdata;

static int __init tdx_host_param(char *str)
{
	if (str && !strcmp(str, "on"))
		tdx_host = TDX_HOST_ON;

	return 0;
}
early_param("tdx_host", tdx_host_param);

/*
 * is_seamrr_enabled - check if seamrr is supported.
 */
static bool __init is_seamrr_enabled(void)
{
	u64 mtrrcap, seamrr_base, seamrr_mask;

	if (!boot_cpu_has(X86_FEATURE_MTRR))
		return false;

	/* MTRRcap.SEAMRR indicates the support of SEAMRR_PHYS_{BASE, MASK} */
	rdmsrl(MSR_MTRRcap, mtrrcap);
	if (!(mtrrcap & MTRRCAP_SEAMRR))
		return false;

	rdmsrl(MSR_IA32_SEAMRR_PHYS_BASE, seamrr_base);
	if (!(seamrr_base & MSR_IA32_SEAMRR_PHYS_BASE_CONFIGURED)) {
		pr_info("SEAMRR base is not configured by BIOS\n");
		return false;
	}

	rdmsrl(MSR_IA32_SEAMRR_PHYS_MASK, seamrr_mask);
	if (!(seamrr_mask & MSR_IA32_SEAMRR_PHYS_MASK_ENABLED)) {
		pr_info("SEAMRR is not enabled by BIOS\n");
		return false;
	}

	return true;
}

static int __init tdx_host_early_init(void)
{
	int ret;

	/* Avoid TDX overhead when opt-in is not present. */
	if (tdx_host != TDX_HOST_ON)
		return 0;

	/* TDX requires SEAM mode. */
	if (!is_seamrr_enabled())
		return -EOPNOTSUPP;

	/* TDX(SEAMCALL) requires VMX. */
	ret = seam_init_vmx_early();
	if (ret)
		return ret;

	ret = p_seamldr_get_info();
	if (ret == -EIO) {
		pr_err("No P-SEAMLDR loaded by BIOS.\n");

		ret = load_p_seamldr();
		if (ret)
			return ret;

		ret = p_seamldr_get_info();
		if (ret) {
			pr_err("Get P-SEAMLDR failed with %d\n", ret);
			return ret;
		}
	} else if (ret) {
		pr_err("Get P-SEAMLDR failed with %d\n", ret);
		return ret;
	}

	setup_force_cpu_cap(X86_FEATURE_SEAM);
	return tdx_sysmem_build();
}
early_initcall(tdx_host_early_init);

static void pr_seamcall_error(u64 op, const char *op_str,
			      u64 err, struct tdx_ex_ret *ex)
{
	pr_err_ratelimited("SEAMCALL[%s] failed: %s (0x%llx)\n",
			   op_str, tdx_seamcall_error_name(err), err);
	if (ex)
		pr_seamcall_ex_ret_info(op, err, ex);
}

static char tdx_module_name[128] __initdata = "intel-seam/libtdx.so";
static char tdx_sigstruct_name[128] __initdata = "intel-seam/libtdx.so.sigstruct";

static int __init setup_tdx_module(char *str)
{
	if (!str || str[0] == '\0')
		return 0;
	strscpy(tdx_module_name, str, sizeof(tdx_module_name));
	return 1;
}
__setup("tdx_module=", setup_tdx_module);

static int __init setup_tdx_sigstruct(char *str)
{
	if (!str || str[0] == '\0')
		return 0;
	strscpy(tdx_sigstruct_name, str, sizeof(tdx_sigstruct_name));
	return 1;
}
__setup("tdx_sigstruct=", setup_tdx_sigstruct);

static bool trace_boot_seamcalls;

static int __init trace_seamcalls(char *s)
{
	trace_boot_seamcalls = true;
	return 1;
}
__setup("trace_boot_seamcalls", trace_seamcalls);

/*
 * runtime update of TDX module is future task.  Track state of TDX module as
 * preliminary and export the state via sysfs for admin.
 */
enum TDX_MODULE_STATE {
	TDX_MODULE_NOT_LOADED = 0,
	/*
	 * The TDX module is loaded into SEAM region.  Not functional yet until
	 * initialization is done.
	 */
	TDX_MODULE_LOADED,
	/* Initialization is done so that the TDX module is functional. */
	TDX_MODULE_INITIALIZED,
	/* TDX module is already shut down. Futher SEAMCALLs are prevented */
	TDX_MODULE_SHUTDOWN,
	/*
	 * No SEAMCALLs are allowed so that the TDX module is not functional.
	 * It's ready for P-SEAMLDR to update the TDX module.  As something went
	 * wrong, a system reboot would be needed to fix it.
	 */
	TDX_MODULE_ERROR,
};

/* TODO: export the state via sysfs. */
static enum TDX_MODULE_STATE tdx_module_state;

/* Protect tdx_module_state */
static DEFINE_MUTEX(tdx_mutex);

bool is_debug_seamcall_available __read_mostly = true;

bool is_nonarch_seamcall_available __read_mostly = true;

/* TDX system information returned by TDH_SYS_INFO. */
static struct tdsysinfo_struct *tdx_tdsysinfo;

static BLOCKING_NOTIFIER_HEAD(tdx_notify_list);

/* Inform notifiers about an event, expecting no error from notifiers. */
static void tdx_notify(unsigned long val)
{
	int ret;

	ret = blocking_notifier_call_chain(&tdx_notify_list, val, NULL);
	WARN_ON_ONCE(notifier_to_errno(ret));
}

/* Inform notifiers about an event and rollback on error */
static int tdx_notify_rollback(unsigned long val, unsigned long val_rollback)
{
	int ret;

	ret = blocking_notifier_call_chain_robust(&tdx_notify_list, val,
						  val_rollback, NULL);
	return notifier_to_errno(ret);
}

static int set_tdx_module_state(int state)
{
	int ret;

	if (state == tdx_module_state)
		return 0;

	/*
	 * Send notifications when TDX module state changes between
	 * TDX_MODULE_INITIALIZED and other states. When TDX module
	 * is about to go down, notifiers are allowed to return errors
	 * to abort this attempt in case that shutting down a busy TDX
	 * module leads to data loss.
	 */
	if (tdx_module_state == TDX_MODULE_INITIALIZED) {
		ret = tdx_notify_rollback(TDX_MODULE_LOAD_BEGIN,
					  TDX_MODULE_LOAD_DONE);
		if (ret)
			return ret;
	} else if (state == TDX_MODULE_INITIALIZED)
		tdx_notify(TDX_MODULE_LOAD_DONE);

	tdx_module_state = state;
	return 0;
}

static int get_tdx_module_state(void)
{
	return tdx_module_state;
}

/**
 * register_tdx_notifier - subscribe to state change of TDX module
 * @nb: new entry to subscribe in notifier chain
 *
 * Note that the nb::notifier_call is invoked during registration
 * if TDX module is already available.
 */
int register_tdx_notifier(struct notifier_block *nb)
{
	int ret;

	/*
	 * If tdx_module_state gets changed to TDX_MODULE_INITIALIZED after
	 * blocking_notifier_chain_register() but before checking tdx module
	 * state below, a duplicate event will be sent to the notifier. Hold
	 * the mutex to prevent any change to tdx_module_state.
	 */
	mutex_lock(&tdx_mutex);
	ret = blocking_notifier_chain_register(&tdx_notify_list, nb);
	/*
	 * Registering a notifier may happen after TDX module is ready to
	 * function. If that's the case, send a notification now.
	 */
	if (!ret && (get_tdx_module_state() == TDX_MODULE_INITIALIZED))
		nb->notifier_call(nb, TDX_MODULE_LOAD_DONE, NULL);
	mutex_unlock(&tdx_mutex);

	return ret;
}
EXPORT_SYMBOL_GPL(register_tdx_notifier);

/**
 * unregister_tdx_notifier - unsubscribe to state change of TDX module
 * @nb: entry to unsbuscribe from notifier chain
 */
int unregister_tdx_notifier(struct notifier_block *nb)
{
	return blocking_notifier_chain_unregister(&tdx_notify_list, nb);
}
EXPORT_SYMBOL_GPL(unregister_tdx_notifier);

/*
 * Return pointer to TDX system info (TDSYSINFO_STRUCT) if TDX has been
 * successfully initialized, or NULL.
 */
const struct tdsysinfo_struct *tdx_get_sysinfo(void)
{
	return tdx_tdsysinfo;
}
EXPORT_SYMBOL_GPL(tdx_get_sysinfo);	/* kvm_intel will use this. */

/* CMR info array returned by TDH_SYS_INFO. */
static struct cmr_info *tdx_cmrs;
static int tdx_nr_cmrs;

/* KeyID range reserved to TDX by BIOS */
u32 tdx_keyids_start __read_mostly;
EXPORT_SYMBOL_GPL(tdx_keyids_start);	/* kvm_intel will use this. */
u32 tdx_nr_keyids __read_mostly;
EXPORT_SYMBOL_GPL(tdx_nr_keyids);	/* kvm_intel will use this. */
u32 tdx_seam_keyid __read_mostly;
EXPORT_SYMBOL_GPL(tdx_seam_keyid);	/* kvm_intel will use this. */

static void tdx_get_keyids(u32 *keyids_start, u32 *nr_keyids)
{
	u32 nr_mktme_ids;

	rdmsr(MSR_IA32_MKTME_KEYID_PART, nr_mktme_ids, *nr_keyids);

	/* KeyID 0 is reserved, i.e. KeyIDs are 1-based. */
	*keyids_start = nr_mktme_ids + 1;
}

static int *tdx_package_leaders __read_mostly;
static int *tdx_package_leaders_nmi __read_mostly;

static enum cpuhp_state cpuhp_state __read_mostly = CPUHP_INVALID;

static int tdx_starting_cpu(unsigned int cpu)
{
	int pkg = topology_physical_package_id(cpu);
	int ret = 0;

	/*
	 * If this package doesn't have a leader CPU for IPI operation, use this
	 * CPU as package leader.
	 */
	if (tdx_package_leaders[pkg] == -1) {
		tdx_package_leaders[pkg] = cpu;
		tdx_package_leaders_nmi[pkg] = -1;
	}

	return ret;
}

static int tdx_dying_cpu(unsigned int cpu)
{
	int pkg = topology_physical_package_id(cpu);
	int other;

	if (tdx_package_leaders[pkg] != cpu)
		return 0;

	/*
	 * If offlining cpu is used as package leader, find other online cpu on
	 * this package.
	 */
	tdx_package_leaders[pkg] = -1;
	for_each_online_cpu(other) {
		if (other == cpu)
			continue;
		if (topology_physical_package_id(other) != pkg)
			continue;

		tdx_package_leaders[pkg] = other;
		break;
	}
	/*
	 * Some of the TDX module API (tdh.sys.key.config, tdh.mng.key.config
	 * and, tdh.phymem.page.wbinvd) require to invoke on all the CPU
	 * package. If all CPUs in the package are offline, resort to use NMI
	 * to invoke a handler on the package.
	 */
	if (tdx_package_leaders[pkg] == -1)
		tdx_package_leaders_nmi[pkg] = cpu;

	return 0;
}

static int __init tdx_init_cpuhp(void)
{
	int ret;

	ret = cpuhp_setup_state_nocalls_cpuslocked(CPUHP_AP_ONLINE_DYN,
		"tdx/cpu:starting", tdx_starting_cpu, tdx_dying_cpu);
	if (ret >= 0) {
		cpuhp_state = ret;
		ret = 0;
	}
	return ret;
}

static int (*tdx_nmi_fn)(void *param);
static void *tdx_nmi_param;
static int tdx_nmi_ret;
static bool tdx_nmi_finished;

static int tdx_nmi_handler(unsigned int cmd, struct pt_regs *regs)
{
	int cpu = smp_processor_id();
	int pkg = topology_physical_package_id(cpu);

	if (tdx_package_leaders_nmi[pkg] != cpu)
		return NMI_DONE;

	rmb();	/* Read the values set by tdx_trigger_nmi_handler(). */
	tdx_nmi_ret = tdx_nmi_fn(tdx_nmi_param);

	wmb();	/* Publish return value before marking finished. */
	WRITE_ONCE(tdx_nmi_finished, true);
	return NMI_HANDLED;
}

static int tdx_trigger_nmi_handler(int cpu, int (*fn)(void *param), void *param)
{
	tdx_nmi_fn = fn;
	tdx_nmi_param = param;
	tdx_nmi_finished = false;
	/* Ensure those variables are set before sending out the NMI. */
	wmb();

	apic->send_IPI(cpu, NMI_VECTOR);
	while (!READ_ONCE(tdx_nmi_finished))
		cpu_relax();
	rmb();	/* Ensure the return value is the one stored above. */
	return READ_ONCE(tdx_nmi_ret);
}

/*
 * Setup one-cpu-per-pkg array to do package-scoped SEAMCALLs. The array is
 * only necessary if there are multiple packages.
 */
static int __init init_package_leaders(void)
{
	int cpu, pkg, nr_filled, nr_pkgs;

	nr_pkgs = topology_max_packages();
	tdx_package_leaders = kcalloc(nr_pkgs, sizeof(*tdx_package_leaders),
				GFP_KERNEL);
	tdx_package_leaders_nmi = kcalloc(
		nr_pkgs, sizeof(*tdx_package_leaders_nmi), GFP_KERNEL);
	if (!tdx_package_leaders || !tdx_package_leaders_nmi) {
		/* kfree is NULL-safe. */
		kfree(tdx_package_leaders);
		kfree(tdx_package_leaders_nmi);
		tdx_package_leaders = NULL;
		tdx_package_leaders_nmi = NULL;
		return -ENOMEM;
	}

	memset(tdx_package_leaders, -1, nr_pkgs * sizeof(*tdx_package_leaders));
	memset(tdx_package_leaders_nmi, -1,
	       nr_pkgs * sizeof(*tdx_package_leaders_nmi));

	nr_filled = 0;
	for_each_online_cpu(cpu) {
		pkg = topology_physical_package_id(cpu);
		if (tdx_package_leaders[pkg] >= 0)
			continue;

		tdx_package_leaders[pkg] = cpu;
		if (++nr_filled == nr_pkgs)
			break;
	}

	if (WARN_ON(nr_filled != nr_pkgs)) {
		kfree(tdx_package_leaders);
		tdx_package_leaders = NULL;
		return -EIO;
	}

	return register_nmi_handler(NMI_LOCAL, tdx_nmi_handler, 0,
				"tdx_nmi_handler");
}

static int tdx_seamcall_on_each_pkg_cpuslocked(int (*fn)(void *), void *param)
{
	int ret, i;

	/*
	 * Some per-package operations require serialization between packages.
	 * Invoke the callback one by one to avoid watchdog timer.
	 */
	ret = 0;
	for (i = 0; i < topology_max_packages(); i++) {
		if (tdx_package_leaders[i] < 0)
			ret = tdx_trigger_nmi_handler(
				tdx_package_leaders_nmi[i], fn, param);
		else
			ret = smp_call_on_cpu(
				tdx_package_leaders[i], fn, param, 1);
		if (ret)
			break;
	}

	return ret;
}

/*
 * tdx_seamcall_on_each_pkg - run function on each packages serially.
 * @fn: function to be called on each packages in blocking manner.
 * @param: parameter for fn
 */
int tdx_seamcall_on_each_pkg(int (*fn)(void *), void *param)
{
	int ret;

	/* protect tdx_package_leaders. */
	cpus_read_lock();
	ret = tdx_seamcall_on_each_pkg_cpuslocked(fn, param);
	cpus_read_unlock();

	return ret;
}
/* kvm_intel will use this function to invoke SEAMCALL on each package. */
EXPORT_SYMBOL_GPL(tdx_seamcall_on_each_pkg);

/*
 * TDH_SYS_CONFIG requires that struct tdsysinfo_struct and the array of struct
 * cmr_info have the alignment of TDX_TDSYSINFO_STRUCT_ALIGNEMNT(1024) and
 * TDX_CMR_INFO_ARRAY_ALIGNMENT(512).
 * sizeof(struct tdsysinfo_struct) = 1024
 * sizeof(struct cmr_info) * TDX_MAX_NR_CMRS = 512
 *
 * NOTE: kmalloc() returns size-aligned when size of power of 2.
 */
static int tdx_sys_info_alloc(struct tdsysinfo_struct **tdsysinfo,
			      struct cmr_info **cmrs)
{
	/* tdh_sys_info() requires special alignment. */
	BUILD_BUG_ON(sizeof(struct tdsysinfo_struct) != 1024);
	BUILD_BUG_ON(!is_power_of_2(sizeof(**tdsysinfo)));
	BUILD_BUG_ON(!IS_ALIGNED(sizeof(**tdsysinfo),
				 TDX_TDSYSINFO_STRUCT_ALIGNEMNT));
	BUILD_BUG_ON(!is_power_of_2(sizeof(**cmrs) * TDX_MAX_NR_CMRS));
	BUILD_BUG_ON(!IS_ALIGNED(sizeof(**cmrs) * TDX_MAX_NR_CMRS,
				 TDX_CMR_INFO_ARRAY_ALIGNMENT));

	*tdsysinfo = kzalloc(sizeof(**tdsysinfo), GFP_KERNEL);
	*cmrs = kzalloc(sizeof(**cmrs) * TDX_MAX_NR_CMRS,
			GFP_KERNEL | __GFP_ZERO);
	if (!*tdsysinfo || !*cmrs) {
		/* kfree() is NULL-safe. */
		kfree(*tdsysinfo);
		kfree(*cmrs);
		*tdsysinfo = NULL;
		*cmrs = NULL;
		return -ENOMEM;
	}
	return 0;
}

/*
 * free_seamldr_params - free allocated for seamldr_params including referenced
 *			 pages by params.
 * @params: virtual address of struct seamldr_params to free
 */
static void free_seamldr_params(struct seamldr_params *params)
{
	int i;

	if (!params)
		return;

	for (i = 0; i < params->num_module_pages; i++)
		free_page((unsigned long)__va(params->mod_pages_pa_list[i]));
	free_page((unsigned long)__va(params->sigstruct_pa));
	free_page((unsigned long)params);
}

/*
 * alloc_seamldr_params - initialize parameters for P-SEAMLDR to load TDX module.
 * @module: virtual address of TDX module.
 * @module_size: size of module.
 * @sigstruct: virtual address of sigstruct of TDX module.
 * @sigstruct_size: size of sigstruct of TDX module.
 * @scenario: SEAMLDR_SCENARIO_LOAD or SEAMLDR_SCENARIO_UPDATE.
 * @return: pointer to struct seamldr_params on success, error code on failure.
 *
 * Allocate and initialize struct seamldr_params for P-SEAMLDR to load TDX
 * module.  Memory for seamldr_params and members is required to be 4K
 * page-aligned.  Use free_seamldr_params() to free allocated pages including
 * referenced by params.
 *
 * KASAN thinks memcpy from initrd image via cpio image invalid access.
 * Here module and sigstruct come from initrd image, not from memory allocator.
 * Annotate it with __no_sanitize_address to apiece KASAN.
 */
static struct seamldr_params *__no_sanitize_address alloc_seamldr_params(
	const void *module, unsigned long module_size, const void *sigstruct,
	unsigned long sigstruct_size, u64 scenario)
{
	struct seamldr_params *params = NULL;
	void *sigstruct_page = NULL;
	void *module_page = NULL;
	int i;

	BUILD_BUG_ON(SEAMLDR_SIGSTRUCT_SIZE > PAGE_SIZE);

	/*
	 * SEAM module must be equal or less than
	 * SEAMLDR_MAX_NR_MODULE_PAGES(496) pages.
	 */
	if (!module_size ||
	    module_size > SEAMLDR_MAX_NR_MODULE_PAGES * PAGE_SIZE) {
		pr_err("Invalid SEAM module size 0x%lx\n", module_size);
		return ERR_PTR(-EINVAL);
	}
	/*
	 * SEAM signature structure must be SEAMLDR_SIGSTRUCT_SIZE(2048) bytes.
	 */
	if (sigstruct_size != SEAMLDR_SIGSTRUCT_SIZE) {
		pr_err("Invalid SEAM signature structure size 0x%lx\n",
		       sigstruct_size);
		return ERR_PTR(-EINVAL);
	}

	/*
	 * Allocate and initialize the SEAMLDR params.  Pages are passed in as
	 * a list of physical addresses.
	 */
	params = (struct seamldr_params *)get_zeroed_page(GFP_KERNEL);
	if (!params) {
		pr_err("Unable to allocate memory for SEAMLDR_PARAMS\n");
		goto out;
	}
	params->scenario = scenario;

	/* SEAMLDR requires the sigstruct to be 4K aligned. */
	sigstruct_page = (void *)__get_free_page(GFP_KERNEL);
	if (!sigstruct_page) {
		pr_err("Unable to allocate memory to copy sigstruct\n");
		goto out;
	}
	memcpy(sigstruct_page, sigstruct, sigstruct_size);
	params->sigstruct_pa = __pa(sigstruct_page);

	params->num_module_pages = PFN_UP(module_size);
	for (i = 0; i < params->num_module_pages; i++) {
		module_page = (void *)__get_free_page(GFP_KERNEL);
		if (!module_page) {
			pr_err("Unable to allocate memory to copy SEAM module\n");
			goto out;
		}
		params->mod_pages_pa_list[i] = __pa(module_page);
		memcpy(module_page, module + i * PAGE_SIZE,
		       min(module_size, PAGE_SIZE));
		if (module_size < PAGE_SIZE)
			memset(module_page + module_size, 0,
			       PAGE_SIZE - module_size);
		module_size -= PAGE_SIZE;
	}

	return params;

out:
	free_seamldr_params(params);
	return ERR_PTR(-ENOMEM);
}

struct tdx_install_module_data {
	struct seamldr_params *params;
	atomic_t error;
};

/* Load seam module on one CPU */
static void tdx_install_module_cpu(void *data)
{
	struct tdx_install_module_data *install_module = data;
	int ret = seamldr_install(__pa(install_module->params));

	if (ret)
		atomic_set(&install_module->error, ret);
}

static int tdx_init_lp(void)
{
	u32 keyids_start, nr_keyids;
	struct tdx_ex_ret ex_ret;
	u64 err;

	WARN_ON(!irqs_disabled());

	/*
	 * MSR_IA32_MKTME_KEYID_PART is core-scoped, disable TDX if this CPU's
	 * partitioning doesn't match the BSP's partitioning.
	 */
	tdx_get_keyids(&keyids_start, &nr_keyids);
	if (keyids_start != tdx_keyids_start || nr_keyids != tdx_nr_keyids) {
		pr_err("MKTME KeyID partioning inconsistent on CPU %u\n",
		       smp_processor_id());
		return -EIO;
	}

	err = tdh_sys_lp_init(&ex_ret);
	if (WARN_ON_ONCE(err)) {
		pr_seamcall_error(SEAMCALL_TDH_SYS_LP_INIT, "TDH_SYS_LP_INIT",
				  err, &ex_ret);
		return -EIO;
	}

	return 0;
}

static void tdx_init_cpu(void *data)
{
	atomic_t *error = data;
	int ret = tdx_init_lp();

	if (ret)
		atomic_set(error, ret);
}

/*
 * Invoke TDH.SYS.LP.INIT on all CPUs to perform processor-wide initialization.
 */
static int tdx_init_cpus(void)
{
	atomic_t error;

	/* Call per-CPU initialization function on all CPUs. */
	atomic_set(&error, 0);
	on_each_cpu(tdx_init_cpu, &error, 1);
	/* Don't care what exact errors occurred on which cpus. */
	return atomic_read(&error);
}

/*
 * tdx_init_system - system wide initialization of TDX module.
 * @return: 0 on success, error code on failure.
 *
 * Does system wide initialization of TDX module.
 */
static int tdx_init_system(void)
{
	struct tdx_ex_ret ex_ret;
	u64 err;

	/*
	 * Detect HKID for TDX if initialization was successful.
	 *
	 * TDX provides core-scoped MSR for us to simply read out TDX start
	 * keyID and number of keyIDs.
	 */
	tdx_get_keyids(&tdx_keyids_start, &tdx_nr_keyids);
	if (!tdx_nr_keyids)
		return -EOPNOTSUPP;

	/* System wide initialization for TDX module. */
	err = tdh_sys_init(0, &ex_ret);
	if (WARN_ON_ONCE(err)) {
		pr_seamcall_error(SEAMCALL_TDH_SYS_INIT, "TDH_SYS_INIT",
				  err, &ex_ret);
		return -EIO;
	}

	/*
	 * Per-CPU initialization.  tdh_sys_info() below requires that LP is
	 * initialized for TDX module.  Otherwise it results in an error,
	 * TDX_SYSINITLP_NOT_DONE.
	 */
	return tdx_init_cpus();
}

/*
 * tdx_get_system_info - store TDX system information into the following
 *                       variables. tdx_keyid_start, tdx_nr_keyids,
 *                       tdx_tdsysinfo, tdx_cmrs and tdx_nr_cmrs.
 *
 * @return: 0 on success, error code on failure.
 *
 * get info about system. i.e. info about TDX module and Convertible Memory
 * Regions(CMRs).
 */
static int tdx_get_system_info(void)
{
	struct tdx_ex_ret ex_ret;
	u64 err;
	int i;

	/*
	 * Invoke TDH_SYS_INFO to collect the information needed to construct
	 * TDMRs.
	 */
	err = tdh_sys_info(__pa(tdx_tdsysinfo), sizeof(*tdx_tdsysinfo),
			   __pa(tdx_cmrs), TDX_MAX_NR_CMRS, &ex_ret);
	if (WARN_ON(err)) {
		pr_seamcall_error(SEAMCALL_TDH_SYS_INFO, "TDH_SYS_INFO",
				  err, &ex_ret);
		return -EIO;
	}

	/*
	 * ex_ret.nr_cmr_entries is how many entries TDX module writes.  It may
	 * contain 0-size entries at the end.  Count non 0-size entries.
	 */
	tdx_nr_cmrs = 0;
	for (i = 0; i < ex_ret.sys_info.nr_cmr_entries; i++) {
		if (!tdx_cmrs[i].size)
			break;
		tdx_nr_cmrs++;
	}

	/*
	 * Sanity check TDSYSINFO.  TDX module should have the architectural
	 * values in TDX spec.
	 */
	if (((tdx_tdsysinfo->max_reserved_per_tdmr != TDX_MAX_NR_RSVD_AREAS) ||
		(tdx_tdsysinfo->max_tdmrs != TDX_MAX_NR_TDMRS) ||
		(tdx_tdsysinfo->pamt_entry_size != TDX_PAMT_ENTRY_SIZE))) {
		pr_err("Invalid TDSYSINFO.  Disable TDX.\n");
		return -EINVAL;
	}

	pr_info("TDX SEAM module: attributes 0x%x vendor_id 0x%x build_date %d "
		"build_num 0x%x minor_version 0x%x major_version 0x%x.\n",
		tdx_tdsysinfo->attributes, tdx_tdsysinfo->vendor_id,
		tdx_tdsysinfo->build_date, tdx_tdsysinfo->build_num,
		tdx_tdsysinfo->minor_version, tdx_tdsysinfo->major_version);

	/* Keep tdx_tdsysinfo to export that info via sysfs. */

	for (i = 0; i < tdx_nr_cmrs; i++)
		pr_info("TDX CMR[%2d]: base 0x%016llx size 0x%016llx\n",
			i, tdx_cmrs[i].base, tdx_cmrs[i].size);

	return 0;
}

static void tdx_shutdown_cpu(void *data)
{
	u64 err;
	int ret = 0;

	WARN_ON_ONCE(!irqs_disabled());

	err = tdh_sys_lp_shutdown();
	if (WARN_ON_ONCE(err)) {
		pr_seamcall_error(SEAMCALL_TDH_SYS_LP_SHUTDOWN,
				  "TDH_SYS_LP_SHUTDOWN", err, NULL);
		ret = -EIO;
	}

	/* Don't care what exact errors occurred on which cpus. */
	if (ret)
		atomic_set((atomic_t *)data, ret);
}

static int tdx_module_shutdown(void)
{
	atomic_t error;
	int ret, state;

	state = get_tdx_module_state();
	if (state != TDX_MODULE_INITIALIZED && state != TDX_MODULE_LOADED)
		return 0;

	/*
	 * TDX module cannot function after entering to shutdown state. Set
	 * tdx_module_state first which may inform registered notifiers. It
	 * gives notifiers a chance to block this action.
	 */
	ret = set_tdx_module_state(TDX_MODULE_SHUTDOWN);
	if (ret)
		return ret;

	atomic_set(&error, 0);
	on_each_cpu(tdx_shutdown_cpu, &error, 1);
	ret = atomic_read(&error);
	if (ret)
		set_tdx_module_state(TDX_MODULE_ERROR);

	return ret;
}

/*
 * tdx_load_module - load TDX module by P-SEAMLDR seam_install call.
 * @module: virtual address of TDX module.
 * @module_size: size of TDX module.
 * @sigstruct: virtual address of sigstruct of TDX module.
 * @sigstruct_size: size of sigstruct of TDX module.
 * @scenario: SEAMLDR_SCENARIO_LOAD or SEAMLDR_SCENARIO_UPDATE.
 * @return: 0 on success, error code on failure.
 *
 * load TDX module on all CPUs through P-SEAMLDR and does get system info about
 * TDX module.
 */
static int tdx_load_module(
	const void *module, unsigned long module_size, const void *sigstruct,
	unsigned long sigstruct_size, u64 scenario)
{
	struct seamldr_params *params;
	struct tdx_install_module_data install_module;
	int cpu;
	int ret = 0;

	params = alloc_seamldr_params(module, module_size, sigstruct,
				      sigstruct_size, scenario);
	if (IS_ERR(params))
		return -ENOMEM;

	ret = tdx_module_shutdown();
	if (ret)
		goto out;

	install_module.params = params;
	atomic_set(&install_module.error, 0);
	/*
	 * Call the function on each CPUs one by one to avoid NMI watchdog.  If
	 * there are many CPUs, tdx_install_module_cpu() may contend with the
	 * spinlock of seamldr_install() for long time to trigger NMI watchdog.
	 */
	for_each_online_cpu(cpu) {
		smp_call_function_single(cpu, tdx_install_module_cpu,
					&install_module, 1);
		/* don't care what exact error occurred on which cpus. */
		ret = atomic_read(&install_module.error);
		if (ret)
			break;
	}
out:
	free_seamldr_params(params);
	return ret;
}

static int __init tdx_load_module_boot(void)
{
	struct cpio_data module, sigstruct;

	pr_info("Loading TDX module via P-SEAMLDR with %s and %s\n",
		tdx_module_name, tdx_sigstruct_name);

	if (!seam_get_firmware(&module, tdx_module_name) ||
	    !seam_get_firmware(&sigstruct, tdx_sigstruct_name)) {
		pr_err("no TDX module or sigstruct found %s/%s\n",
		       tdx_module_name, tdx_sigstruct_name);
		return -ENOENT;
	}

	return tdx_load_module(module.data, module.size, sigstruct.data,
			       sigstruct.size, SEAMLDR_SCENARIO_LOAD);
}

/*
 * Look for seam module binary in built-in firmware and initrd, and load it on
 * all CPUs through P-SEAMLDR.
 */
static int __init tdx_arch_init(void)
{
	int vmxoff_err;
	int ret = 0;

	if (!boot_cpu_has(X86_FEATURE_SEAM))
		goto out_free;

	ret = tdx_sys_info_alloc(&tdx_tdsysinfo, &tdx_cmrs);
	if (ret)
		return -ENOMEM;

	/*
	 * Because smp is enabled, prevent potential concurrent cpu
	 * online/offline.
	 */
	cpus_read_lock();

	/*
	 * Initialization of TDX module needs to involve all CPUs.  Ensure all
	 * CPUs are online.  All CPUs are required to be initialized by
	 * TDH.SYS.LP.INIT otherwise TDH.SYS.CONFIG fails.
	 */
	if (!cpumask_equal(cpu_present_mask, cpu_online_mask)) {
		ret = -EINVAL;
		goto out_unlock;
	}

	mutex_lock(&tdx_mutex);

	/* SEAMCALL requires to enable VMXON on CPUs. */
	ret = seam_vmxon_on_each_cpu();
	if (ret)
		goto out;

	if (!tdx_module_loaded()) {
		pr_err("No TDX module loaded by BIOS.\n");

		ret = tdx_load_module_boot();
		if (ret) {
			pr_info("Failed to load TDX module.\n");
			goto out;
		}
		pr_info("Loaded TDX module via P-SEAMLDR.\n");
	} else if ((bootloader_type >> 4) == 0xd){
		pr_info("It's a kexec'ed kernel, loading new TDX module kernel itself to overwrite old one\n");

		ret = tdx_load_module_boot();
		if (ret) {
			pr_info("Failed to load TDX module.\n");
			goto out;
		}
		pr_info("Loaded TDX module via P-SEAMLDR.\n");
	}
	set_tdx_module_state(TDX_MODULE_LOADED);

	ret = tdx_init_system();
	if (ret)
		goto out;

	ret = tdx_get_system_info();
	if (ret)
		goto out;

	ret = init_package_leaders();
	if (ret)
		goto out;

	ret = tdx_init_cpuhp();
out:
	/*
	 * Other codes (Especially kvm_intel) expect that they're the first to
	 * use VMX.  That is, VMX is off on their initialization.  Maintain the
	 * assumption to keep them working.
	 */
	vmxoff_err = seam_vmxoff_on_each_cpu();
	if (vmxoff_err) {
		pr_info("Failed to VMXOFF.\n");
		if (!ret)
			ret = vmxoff_err;
	}
	if (ret)
		set_tdx_module_state(TDX_MODULE_ERROR);
	mutex_unlock(&tdx_mutex);
out_unlock:
	cpus_read_unlock();

	if (ret && cpuhp_state != CPUHP_INVALID) {
		cpuhp_remove_state_nocalls(cpuhp_state);
		cpuhp_state = CPUHP_INVALID;
	}
out_free:
	return ret;
}
/*
 * arch_initcall() is chosen to satisfy the following conditions.
 * - After P-SEAMLDR is loaded.
 * - After SMP initialization.
 */
arch_initcall(tdx_arch_init);

/* Array of all TDMR info array. */
static struct tdmr_info *tdmr_info; /* aligned to TDX_TDMR_INFO_ALIGNMENT. */
/* Number of actual TDMRs */
static int tdx_nr_tdmrs;

/* data structure for tdx_init_tdmrs() */
struct tdx_tdmr_init_data {
	struct mutex lock;
	int next_tdmr_index;
	int nr_initialized_tdmrs;
	int failed;
	int nr_completed;
	int nr_works;
	struct completion completion;
};

struct tdx_tdmr_init_request {
	struct work_struct work;
	struct tdx_tdmr_init_data *data;
};

/*
 * __tdx_init_tdmrs - worker to initialize TDMRs
 * @work: work_struct to work queue which embedded in tdx_tdmr_init_request.
 *
 * Get an uninitialized TDMR, initialize it and loop until all TDMRs are
 * initialized.
 */
static void __tdx_init_tdmrs(struct work_struct *work)
{
	struct tdx_tdmr_init_request *req = container_of(
		work, struct tdx_tdmr_init_request, work);
	struct tdx_tdmr_init_data *data = req->data;
	struct tdx_ex_ret ex_ret;
	bool completed;
	u64 base, size;
	u64 err = 0;
	int i;

	mutex_lock(&data->lock);
	while (data->next_tdmr_index < tdx_nr_tdmrs) {
		i = data->next_tdmr_index++;
		base = tdmr_info[i].base;
		size = tdmr_info[i].size;

		while (true) {
			/* Abort if a different CPU failed. */
			if (data->failed)
				goto out;

			mutex_unlock(&data->lock);
			err = tdh_sys_tdmr_init(base, &ex_ret);
			if (WARN_ON_ONCE(err)) {
				pr_seamcall_error(SEAMCALL_TDH_SYS_TDMR_INIT,
						  "TDH_SYS_TDMR_INIT", err,
						  &ex_ret);
				err = -EIO;
				mutex_lock(&data->lock);
				goto out;
			}
			cond_resched();
			mutex_lock(&data->lock);

			/*
			 * Note, "next" is simply an indicator, base is passed
			 * to TDH.SYS.TDMR.INIT on every iteration.
			 */
			if (!(ex_ret.sys_tdmr_init.next < (base + size)))
				break;
		}

		data->nr_initialized_tdmrs++;
	}

out:
	if (err)
		data->failed++;
	data->nr_completed++;
	completed = (data->nr_completed == data->nr_works);
	mutex_unlock(&data->lock);

	if (completed)
		complete(&data->completion);
}

/*
 * tdx_init_tdmrs - Initializes TDMRs in parallel way.
 * @return: 0 on success, error code on failure.
 *
 * It may take long time to initialize TDMRs by TDH.SYS.TDMR.INIT that
 * initializes Physical Address Metadata Table(PAMT) which is something similar
 * to Linux struct page.  Parallelize it to shorten boot time by work queue.
 */
static int tdx_init_tdmrs(void)
{
	/*
	 * One TDMR can be initialized only by one thread.  No point to have
	 * threads more than the number of TDMRs.
	 */
	int nr_works = min_t(int, num_online_cpus(), tdx_nr_tdmrs);
	struct tdx_tdmr_init_data data = {
		.next_tdmr_index = 0,
		.nr_initialized_tdmrs = 0,
		.failed = 0,
		.nr_completed = 0,
		.nr_works = nr_works,
		.completion = COMPLETION_INITIALIZER_ONSTACK(data.completion),
	};
	int i;

	struct tdx_tdmr_init_request *reqs = kcalloc(nr_works, sizeof(*reqs),
						     GFP_KERNEL);
	if (!reqs)
		return -ENOMEM;

	mutex_init(&data.lock);
	for (i = 0; i < nr_works; i++) {
		reqs[i].data = &data;
		INIT_WORK(&reqs[i].work, __tdx_init_tdmrs);
		queue_work(system_unbound_wq, &reqs[i].work);
	}
	wait_for_completion(&data.completion);

	kfree(reqs);
	mutex_lock(&data.lock);
	if (data.failed || data.nr_initialized_tdmrs < tdx_nr_tdmrs) {
		mutex_unlock(&data.lock);
		return -EIO;
	}
	mutex_unlock(&data.lock);
	return 0;
}

static int do_tdh_sys_key_config(void *param)
{
	u64 err;

	do {
		err = tdh_sys_key_config();
	} while (err == TDX_KEY_GENERATION_FAILED);
	if (WARN_ON_ONCE(err)) {
		pr_seamcall_error(SEAMCALL_TDH_SYS_KEY_CONFIG,
				  "TDH_SYS_KEY_CONFIG", err, NULL);
		return -EIO;
	}

	return 0;
}

/*
 * __tdx_init_module - finial initialization of TDX module so that it can be
 *                     workable.
 */
static int __tdx_init_module(void)
{
	u64 *tdmr_addrs;
	u64 err;
	int ret = 0;
	int i;

	/*
	 * tdmr_addrs must be aligned to TDX_TDMR_ADDR_ALIGNMENT(512).
	 * kmalloc() returns size-aligned when size is power of 2.
	 */
	BUILD_BUG_ON(!is_power_of_2(sizeof(*tdmr_addrs) * TDX_MAX_NR_TDMRS));
	BUILD_BUG_ON(!IS_ALIGNED(sizeof(*tdmr_addrs) * TDX_MAX_NR_TDMRS,
				 TDX_TDMR_ADDR_ALIGNMENT));
	tdmr_addrs = kmalloc(sizeof(*tdmr_addrs) * TDX_MAX_NR_TDMRS, GFP_KERNEL);
	if (!tdmr_addrs)
		return -ENOMEM;

	for (i = 0; i < tdx_nr_tdmrs; i++)
		tdmr_addrs[i] = __pa(&tdmr_info[i]);

	/*
	 * tdh_sys_tdmr_config() calls TDH.SYS.CONFIG to tell TDX module about
	 * TDMRs, PAMTs and HKID for TDX module to use.  Use the first keyID as
	 * TDX-SEAM's global key.
	 */
	err = tdh_sys_tdmr_config(__pa(tdmr_addrs), tdx_nr_tdmrs,
				  tdx_keyids_start);
	if (WARN_ON_ONCE(err)) {
		pr_seamcall_error(SEAMCALL_TDH_SYS_CONFIG, "TDH_SYS_CONFIG",
				  err, NULL);
		ret = -EIO;
		goto out;
	}
	tdx_seam_keyid = tdx_keyids_start;

	/*
	 * Cache Flush is required as
	 * TDX module spec: Chapter 12 Intel TDX Module Lifecycle Table 12.1
	 */
	wbinvd_on_all_cpus();

	/* Cpuslock is already held by the caller. */
	ret = tdx_seamcall_on_each_pkg_cpuslocked(do_tdh_sys_key_config, NULL);
	if (ret)
		goto out;

	/*
	 * Detect if debug and non-arch seamcall available.
	 *
	 * Even though tracing level is ALL level by default, it needs to set
	 * it explicitly to check if debug seamcall available.
	 */
	if (trace_boot_seamcalls)
		tdh_trace_seamcalls(DEBUGCONFIG_TRACE_ALL);
	else
		/*
		 * Tracing is on by default, disable it before INITTDMR which
		 * causes too many debug messages to take long time.
		 */
		tdh_trace_seamcalls(DEBUGCONFIG_TRACE_CUSTOM);
	/* Unconditionally intercept triple faults to aid debug. */
	tdxmode(true, BIT_ULL(EXIT_REASON_TRIPLE_FAULT));

	ret = tdx_init_tdmrs();
out:
	kfree(tdmr_addrs);
	return ret;
}

static int __init tdx_init_module(void)
{
	struct tdx_module_descriptor desc;
	int ret = 0;

	/*
	 * tdmr_info must be aligned to TDX_TDMR_INFO_ALIGNMENT(512).
	 * NOTE: kmalloc() returns size-aligned when size of power of 2.
	 */
	BUILD_BUG_ON(sizeof(*tdmr_info) != 512);
	BUILD_BUG_ON((sizeof(*tdmr_info) % TDX_TDMR_INFO_ALIGNMENT) != 0);
	tdmr_info = kcalloc(tdx_tdsysinfo->max_tdmrs, sizeof(*tdmr_info),
			GFP_KERNEL);
	if (!tdmr_info) {
		ret = -ENOMEM;
		goto out;
	}

	/* construct all TDMRs */
	desc.max_tdmr_num = tdx_tdsysinfo->max_tdmrs;
	desc.pamt_entry_size[TDX_PG_4K] = tdx_tdsysinfo->pamt_entry_size;
	desc.pamt_entry_size[TDX_PG_2M] = tdx_tdsysinfo->pamt_entry_size;
	desc.pamt_entry_size[TDX_PG_1G] = tdx_tdsysinfo->pamt_entry_size;
	desc.max_tdmr_rsvd_area_num = tdx_tdsysinfo->max_reserved_per_tdmr;

	ret = construct_tdx_tdmrs(tdx_cmrs, tdx_nr_cmrs, &desc, tdmr_info,
			&tdx_nr_tdmrs);
	if (ret)
		goto out;

	/* final initialization to make TDX module workable. */
	ret = __tdx_init_module();
	if (ret)
		goto out;

out:
	return ret;
}

/*
 * The final initialization of the TDX module and make it ready to use.
 */
static int __init tdx_late_init(void)
{
	int vmxoff_err;
	int ret = 0;

	BUILD_BUG_ON(sizeof(struct tdmr_info) != 512);

	mutex_lock(&tdx_mutex);
	if (get_tdx_module_state() != TDX_MODULE_LOADED) {
		mutex_unlock(&tdx_mutex);
		return -ENODEV;
	}

	pr_info("Initializing TDX module.\n");

	/*
	 * Since other subsystem(for example, ACPI subsystem) is initialized,
	 * prevent potential concurrent CPU online/offline.
	 *
	 * - Protect tdx_package_leaders for per-package operation.
	 * - Make seam_vmxon_on_each_cpu() work.  Otherwise concurrently onlined
	 *   CPU has VMX disabled and the SEAM operation on that CPU fails.
	 */
	cpus_read_lock();

	/*
	 * Build legacy PMEMs as TDX memory in subsys_initcall_sync() here,
	 * after e820__reserve_resources_late() is done, since it uses
	 * walk_iomem_res_desc() to find legacy PMEMs
	 */
	ret = tdx_legacy_pmem_build();
	if (ret)
		goto out_err;

	/*
	 * Both TDX memory instances for system memory and legacy PMEMs are
	 * ready.  Merge them into final TDX memory for constructing TDMRs.
	 */
	ret = build_final_tdx_memory();
	if (ret)
		goto out_err;

	/* SEAMCALL requires to enable VMXON on CPUs. */
	ret = seam_vmxon_on_each_cpu();
	if (ret)
		goto out;

	ret = tdx_init_module();
	if (ret)
		goto out;

	pr_info("Successfully initialized TDX module\n");
	set_tdx_module_state(TDX_MODULE_INITIALIZED);

out:
	vmxoff_err = seam_vmxoff_on_each_cpu();
	if (vmxoff_err) {
		pr_info("Failed to VMXOFF.\n");
		if (!ret)
			ret = vmxoff_err;
	}
out_err:
	if (ret) {
		pr_info("Failed to initialize TDX module %d\n", ret);
		set_tdx_module_state(TDX_MODULE_ERROR);
	}
	if (get_tdx_module_state() == TDX_MODULE_INITIALIZED) {
		int cpu;

		setup_force_cpu_cap(X86_FEATURE_TDX);
		for_each_online_cpu(cpu)
			set_cpu_cap(&cpu_data(cpu), X86_FEATURE_TDX);

		tdx_debugfs_init();
	}
	cpus_read_unlock();

	/*
	 * Don't free tdmr_info and tdx_cmrs. tdmr_info can be reused
	 * to configure TDMRs for the new TDX module. tdx_cmrs can be
	 * used to hold CMRs passed from TDX module.
	 */
	if (ret) {
		kfree(tdmr_info);
		kfree(tdx_cmrs);
	}
	cleanup_subtype_tdx_memory();
	mutex_unlock(&tdx_mutex);

	return ret;
}
/*
 * subsys_initcall_sync() is chosen to satisfy the following conditions.
 * - After P-SEAMLDR is loaded.
 * - After the TDX module is loaded.
 * - After iomem_resouce is populated with System RAM including regions
 *   specified by memmap=nn[KMG]!ss[KMG], which is done by
 *   e820_reserve_resources() called by setup_arch().  Because
 *   tdx_construct_tdmr() walks iomem resources looking for legacy pmem region.
 * - After build_sysmem_tdx_memory() by early_initcall().
 * - After reserved memory region is polulated in iomem_resource by
 *   e820__reserve_resources_late(), which is called by
 *   subsys_initcall(pci_subsys_init).
 * - After numa node is initialized by pgdata_init() and alloc_contig_pages() is
 *   available.
 * - Before kvm_intel.  module_init() is mapped to device_initcall() when
 *   it's built into the kernel.
 */
subsys_initcall_sync(tdx_late_init);

#ifdef CONFIG_SYSFS

struct kobject *tdx_kobj;

int __init tdx_sysfs_init(void)
{
	if (!boot_cpu_has(X86_FEATURE_SEAM))
		return 0;

	if (tdx_kobj)
		return 0;

	tdx_kobj = kobject_create_and_add("tdx", firmware_kobj);
	if (!tdx_kobj) {
		pr_err("kobject_create_and_add tdx failed\n");
		return -EINVAL;
	}

	return 0;
}

static struct kobject *tdx_module_kobj;

static ssize_t state_show(
	struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
	static const char * const names[] = {
		[TDX_MODULE_NOT_LOADED] = "not-loaded",
		[TDX_MODULE_LOADED] = "loaded",
		[TDX_MODULE_INITIALIZED] = "initialized",
		[TDX_MODULE_ERROR] = "error"
	};
	const char *state = "unknown";

	if (tdx_module_state < ARRAY_SIZE(names))
		state = names[tdx_module_state];

	return sprintf(buf, "%s\n", state);
}

static struct kobj_attribute tdx_module_state_attr = __ATTR_RO(state);

static struct attribute *tdx_module_states[] = {
	&tdx_module_state_attr.attr,
	NULL,
};

static const struct attribute_group tdx_module_state_group = {
	.attrs = tdx_module_states,
};

#define TDX_MODULE_ATTR_SHOW_FMT(name, fmt)				\
static ssize_t name ## _show(						\
	struct kobject *kobj, struct kobj_attribute *attr, char *buf)	\
{									\
	if (!tdx_tdsysinfo)						\
		return 0;						\
	return sprintf(buf, fmt, tdx_tdsysinfo->name);			\
}									\
static struct kobj_attribute tdx_module_##name = __ATTR_RO(name)

#define TDX_MODULE_ATTR_SHOW_DEC(name)	TDX_MODULE_ATTR_SHOW_FMT(name, "%d\n")
#define TDX_MODULE_ATTR_SHOW_HEX(name)	TDX_MODULE_ATTR_SHOW_FMT(name, "0x%x\n")
#define TDX_MODULE_ATTR_SHOW_U64(name)	TDX_MODULE_ATTR_SHOW_FMT(name, "0x%016llx\n")

TDX_MODULE_ATTR_SHOW_FMT(attributes, "0x%08x\n");
TDX_MODULE_ATTR_SHOW_HEX(vendor_id);
TDX_MODULE_ATTR_SHOW_DEC(build_date);
TDX_MODULE_ATTR_SHOW_HEX(build_num);
TDX_MODULE_ATTR_SHOW_HEX(minor_version);
TDX_MODULE_ATTR_SHOW_HEX(major_version);
TDX_MODULE_ATTR_SHOW_U64(attributes_fixed0);
TDX_MODULE_ATTR_SHOW_U64(attributes_fixed1);
TDX_MODULE_ATTR_SHOW_U64(xfam_fixed0);
TDX_MODULE_ATTR_SHOW_U64(xfam_fixed1);

static struct attribute *tdx_module_attrs[] = {
	&tdx_module_attributes.attr,
	&tdx_module_vendor_id.attr,
	&tdx_module_build_date.attr,
	&tdx_module_build_num.attr,
	&tdx_module_minor_version.attr,
	&tdx_module_major_version.attr,
	&tdx_module_attributes_fixed0.attr,
	&tdx_module_attributes_fixed1.attr,
	&tdx_module_xfam_fixed0.attr,
	&tdx_module_xfam_fixed1.attr,
	NULL,
};

static const struct attribute_group tdx_module_attr_group = {
	.attrs = tdx_module_attrs,
};

/*
 * Search for TDX module and its sigstruct from /lib/firmware and
 * load TDX module through P-SEAMLDR.
 */
int tdx_load_module_late(void)
{
	const struct firmware *module = NULL;
	const struct firmware *sigstruct = NULL;
	char *tdx_module_name = "intel-seam/libtdx.so";
	char *tdx_sigstruct_name = "intel-seam/libtdx.so.sigstruct";
	int ret;

	pr_info("Loading TDX module via P-SEAMLDR with %s and %s\n",
		tdx_module_name, tdx_sigstruct_name);

	ret = request_firmware_direct(&module, tdx_module_name,
				      &tdx_module_dev->dev);
	if (ret)
		return ret;

	ret = request_firmware_direct(&sigstruct, tdx_sigstruct_name,
				      &tdx_module_dev->dev);
	if (ret)
		goto release;

	ret = tdx_load_module(module->data, module->size,
			      sigstruct->data, sigstruct->size,
			      SEAMLDR_SCENARIO_LOAD);
release:
	release_firmware(sigstruct);
	release_firmware(module);
	return ret;
}

int tdx_module_reload(void)
{
	int ret, cpu;

	cpus_read_lock();
	/*
	 * Initialization of TDX module needs to involve all CPUs.  Ensure all
	 * CPUs are online.  All CPUs are required to be initialized by
	 * TDH.SYS.LP.INIT otherwise TDH.SYS.CONFIG fails.
	 */
	if (!cpumask_equal(cpu_present_mask, cpu_online_mask)) {
		ret = -EINVAL;
		goto unlock;
	}

	ret = tdx_load_module_late();
	if (ret)
		goto unlock;

	ret = tdx_init_system();
	if (ret)
		goto unlock;

	ret = tdx_get_system_info();
	if (ret)
		goto unlock;

	ret = __tdx_init_module();
	if (ret)
		goto unlock;

	/* Reset to default values so that their support will be re-probed */
	is_nonarch_seamcall_available = true;
	is_debug_seamcall_available = true;

	pr_info("Successfully initialized TDX module\n");
	set_tdx_module_state(TDX_MODULE_INITIALIZED);

	setup_force_cpu_cap(X86_FEATURE_TDX);
	for_each_online_cpu(cpu)
		set_cpu_cap(&cpu_data(cpu), X86_FEATURE_TDX);

unlock:
	cpus_read_unlock();
	return ret;
}

#define TDX_MODULE_RELOAD	1

static ssize_t tdx_module_reload_store(struct kobject *kobj,
				       struct kobj_attribute *attr,
				       const char *buf, size_t size)
{
	unsigned long val;
	ssize_t ret;
	int vmxoff_err;

	ret = kstrtoul(buf, 0, &val);
	if (ret)
		return ret;

	mutex_lock(&tdx_mutex);
	ret = seam_vmxon_on_each_cpu();
	if (ret)
		goto unlock;

	switch (val) {
	case TDX_MODULE_RELOAD:
		ret = tdx_module_reload();
		break;

	default:
		ret = -EINVAL;
	}
	vmxoff_err = seam_vmxoff_on_each_cpu();
	if (vmxoff_err) {
		pr_info("Failed to VMXOFF.\n");
		if (!ret)
			ret = vmxoff_err;
	}
unlock:
	mutex_unlock(&tdx_mutex);

	if (!ret)
		ret = size;

	return ret;
}

static struct kobj_attribute tdx_attr_module_reload = __ATTR_WO(tdx_module_reload);

static struct attribute *tdx_module_ops[] = {
	&tdx_attr_module_reload.attr,
	NULL,
};

static const struct attribute_group tdx_module_op_group = {
	.attrs = tdx_module_ops,
};

static int __init tdx_module_sysfs_init(void)
{
	int ret = 0;

	if (!boot_cpu_has(X86_FEATURE_SEAM))
		return 0;

	ret = tdx_sysfs_init();
	if (ret)
		return ret;

	tdx_module_dev = platform_device_register_simple("tdx_module", -1, NULL, 0);
	if (IS_ERR(tdx_module_dev))
		return PTR_ERR(tdx_module_dev);

	tdx_module_kobj = kobject_create_and_add("tdx_module", tdx_kobj);
	if (!tdx_module_kobj) {
		pr_err("kobject_create_and_add tdx_module failed\n");
		return -EINVAL;
	}

	ret = sysfs_create_group(tdx_module_kobj, &tdx_module_state_group);
	if (ret) {
		pr_err("Sysfs exporting tdx module state failed %d\n", ret);
		goto err_kobj;
	}

	if (tdx_tdsysinfo) {
		ret = sysfs_create_group(tdx_module_kobj,
					 &tdx_module_attr_group);
		if (ret) {
			pr_err("Sysfs exporting tdx module attributes failed %d\n",
			       ret);
			goto err;
		}

		ret = sysfs_create_group(tdx_module_kobj, &tdx_module_op_group);
		if (ret) {
			pr_err("Sysfs exporting tdx module operations failed %d\n",
			       ret);
			goto err_op;
		}
	}

	return 0;

err_op:
	sysfs_remove_group(tdx_module_kobj, &tdx_module_attr_group);
err:
	sysfs_remove_group(tdx_module_kobj, &tdx_module_state_group);
err_kobj:
	kobject_put(tdx_module_kobj);
	tdx_module_kobj = NULL;
	return ret;
}
device_initcall(tdx_module_sysfs_init);
#endif
