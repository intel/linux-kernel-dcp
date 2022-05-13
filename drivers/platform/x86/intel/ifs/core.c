// SPDX-License-Identifier: GPL-2.0-only
/* Copyright(c) 2021 Intel Corporation.
 *
 * Author: Jithu Joseph <jithu.joseph@intel.com>
 */

#include <linux/cpu.h>
#include <linux/delay.h>
#include <linux/kthread.h>
#include <linux/module.h>
#include <linux/nmi.h>
#include <asm/cpu_device_id.h>

#include "ifs.h"

static enum cpuhp_state cpuhp_scan_state;
struct ifs_params ifs_params;
int cpu_sibl_ct;
atomic_t siblings_in;	/* sibling count for joining rendezvous.*/
atomic_t siblings_out;	/* sibling count for exiting rendezvous.*/
struct completion test_thread_done; /* set when scan are done for all siblings threads.*/

DEFINE_PER_CPU(struct ifs_state, ifs_state);

static int ifs_retry_set(const char *val, const struct kernel_param *kp);
static const struct kernel_param_ops ifs_retry_ops = {
	.set = ifs_retry_set,
	.get = param_get_int,
};

static int retry = 5;
module_param_cb(retry, &ifs_retry_ops, &retry, 0644);

MODULE_PARM_DESC(retry, "Maximum retry count when the test is not executed");

static bool noint = 1;
module_param(noint, bool, 0644);
MODULE_PARM_DESC(noint, "Option to enable/disable interrupt during test");

#define X86_MATCH(model)					\
	X86_MATCH_VENDOR_FAM_MODEL_FEATURE(INTEL, 6,		\
		INTEL_FAM6_##model, X86_FEATURE_CORE_CAPABILITIES, NULL)

static const struct x86_cpu_id ifs_cpu_ids[] __initconst = {
	X86_MATCH(SAPPHIRERAPIDS_X),
	{}
};

MODULE_DEVICE_TABLE(x86cpu, ifs_cpu_ids);

static int ifs_retry_set(const char *val, const struct kernel_param *kp)
{
	int var = 0;

	if (kstrtoint(val, 0, &var)) {
		pr_err("unable to parse retry\n");
		return -EINVAL;
	}

	/* validate retry value for sanity */
	if (var < 1 || var > 20) {
		pr_err("retry parameter should be between 1 and 20\n");
		return -EINVAL;
	}

	return param_set_int(val, kp);
}

static unsigned long msec_to_tsc(unsigned long msec)
{
	return tsc_khz * 1000 * msec / MSEC_PER_SEC;
}

static const char * const scan_test_status[] = {
	"SCAN no error",
	"Other thread could not join.",
	"Interrupt occurred prior to SCAN coordination.",
	"Core Abort SCAN Response due to power management condition.",
	"Non valid chunks in the range",
	"Mismatch in arguments between threads T0/T1.",
	"Core not capable of performing SCAN currently",
	"Unassigned error code 0x7",
	"Exceeded number of Logical Processors (LP) allowed to run Scan-At-Field concurrently",
	"Interrupt occurred prior to SCAN start",
};

static void message_not_tested(int cpu, union ifs_status status)
{
	if (status.error_code < ARRAY_SIZE(scan_test_status))
		pr_warn("CPU %d: SCAN operation did not start. %s\n", cpu,
			scan_test_status[status.error_code]);
	else if (status.error_code == IFS_SW_TIMEOUT)
		pr_warn("CPU %d: software timeout during scan\n", cpu);
	else if (status.error_code == IFS_SW_PARTIAL_COMPLETION)
		pr_warn("CPU %d: %s\n", cpu,
			"Not all scan chunks were executed. Maximum forward progress retries exceeded");
	else
		pr_warn("CPU %d: SCAN unknown status %llx\n", cpu, status.data);
}

static void message_fail(int cpu, union ifs_status status)
{
	if (status.control_error) {
		pr_err("CPU %d: scan failed. %s\n", cpu,
		       "Suggest reload scan file: # echo 1 > /sys/devices/system/cpu/ifs/reload");
	}
	if (status.signature_error) {
		pr_err("CPU %d: test signature incorrect. %s\n", cpu,
		       "Suggest retry scan to check if problem is transient");
	}
}

static bool can_restart(union ifs_status status)
{
	/* Signature for chunk is bad, or scan test failed */
	if (status.signature_error || status.control_error)
		return false;

	switch (status.error_code) {
	case IFS_NO_ERROR:
	case IFS_OTHER_THREAD_DID_NOT_JOIN:
	case IFS_INTERRUPTED_BEFORE_RENDEZVOUS:
	case IFS_POWER_MGMT_INADEQUATE_FOR_SCAN:
	case IFS_EXCEED_NUMBER_OF_THREADS_CONCURRENT:
	case IFS_INTERRUPTED_DURING_EXECUTION:
		return true;
	}
	return false;
}

static bool wait_for_siblings(atomic_t *t, long long timeout)
{
	atomic_inc(t);
	while (atomic_read(t) < cpu_sibl_ct) {
		if (timeout < SPINUNIT) {
			pr_err("Timeout while waiting for CPUs rendezvous, remaining: %d\n",
			       cpu_sibl_ct - atomic_read(t));
			return false;
		}

		ndelay(SPINUNIT);
		timeout -= SPINUNIT;

		touch_nmi_watchdog();
	}

	return true;
}

/*
 * Scan test kthreads bound with each logical cpu.
 * Wait for the sibling thread to join before the execution.
 * Execute the scan test by running wrmsr(MSR_ACTIVATE_SCAN).
 */
static int scan_test_worker(void *info)
{
	int cpu = smp_processor_id();
	union ifs_scan activate;
	union ifs_status status;
	unsigned long timeout;
	int retries;
	u32 first;

	activate.rsvd = 0;
	activate.delay = msec_to_tsc(THREAD_WAIT);
	activate.sigmce = 0;

	while (1) {
		/* wait event until cpumask set from user */
		wait_event_interruptible(per_cpu(ifs_state, cpu).scan_wq,
					 (cpumask_test_cpu(cpu, &per_cpu(ifs_state, cpu).mask) ||
					 kthread_should_stop()));

		if (kthread_should_stop())
			break;

		preempt_disable();
		/* wait for the sibling threads to join */
		first = cpumask_first(topology_sibling_cpumask(cpu));
		if (!wait_for_siblings(&siblings_in, NSEC_PER_SEC)) {
			preempt_enable();
			return -1;
		}

		activate.start = 0;
		activate.stop = ifs_params.valid_chunks - 1;
		timeout = jiffies + HZ / 2;
		retries = retry;

		while (activate.start <= activate.stop) {
			if (time_after(jiffies, timeout))
				break;

			/* disable interrupt during scan if noint set */
			if (noint)
				local_irq_disable();
			/* scan start */
			wrmsrl(MSR_ACTIVATE_SCAN, activate.data);

			if (noint)
				local_irq_enable();

			/*
			 * All logical CPUs on this core are now running IFS test. When it completes
			 * execution or is interrupted, the following RDMSR gets the scan status.
			 */

			rdmsrl(MSR_SCAN_STATUS, status.data);

			/* Some cases can be retried, give up for others */
			if (!can_restart(status))
				break;

			if (status.chunk_num == activate.start) {
				/* Check for forward progress */
				if (retries-- == 0)
					break;
			} else {
				retries = retry;
				activate.start = status.chunk_num;
			}
		}

		preempt_enable();

		/* set s/w defined error code if scan terminated early */
		if (((status.error_code | status.control_error | status.signature_error) == 0) &&
		    activate.start <= activate.stop) {
			if (retries < 0)
				status.error_code = IFS_SW_PARTIAL_COMPLETION;
			else
				status.error_code = IFS_SW_TIMEOUT;
		}

		/* Update status for this core */
		per_cpu(ifs_state, cpu).scan_details = status.data;

		if (status.control_error || status.signature_error) {
			per_cpu(ifs_state, cpu).status = SCAN_TEST_FAIL;
			cpumask_set_cpu(cpu, &ifs_params.fail_mask);
			cpumask_clear_cpu(cpu, &ifs_params.not_tested_mask);
			cpumask_clear_cpu(cpu, &ifs_params.pass_mask);
			message_fail(cpu, status);
		} else if (status.error_code) {
			per_cpu(ifs_state, cpu).status = SCAN_NOT_TESTED;
			cpumask_set_cpu(cpu, &ifs_params.not_tested_mask);
			cpumask_clear_cpu(cpu, &ifs_params.fail_mask);
			cpumask_clear_cpu(cpu, &ifs_params.pass_mask);
			message_not_tested(cpu, status);
		} else {
			per_cpu(ifs_state, cpu).status = SCAN_TEST_PASS;
			cpumask_set_cpu(cpu, &ifs_params.pass_mask);
			cpumask_clear_cpu(cpu, &ifs_params.not_tested_mask);
			cpumask_clear_cpu(cpu, &ifs_params.fail_mask);
		}

		cpumask_clear_cpu(cpu, &per_cpu(ifs_state, cpu).mask);

		if (!wait_for_siblings(&siblings_out, NSEC_PER_SEC))
			return -1;

		if (cpu == first)
			complete(&test_thread_done);
	}

	return 0;
}

static void ifs_first_time(unsigned int cpu)
{
	init_waitqueue_head(&(per_cpu(ifs_state, cpu).scan_wq));

	per_cpu(ifs_state, cpu).first_time = 1;
	per_cpu(ifs_state, cpu).status = SCAN_NOT_TESTED;
	cpumask_set_cpu(cpu, &ifs_params.not_tested_mask);
	cpumask_clear_cpu(cpu, &ifs_params.fail_mask);
	cpumask_clear_cpu(cpu, &ifs_params.pass_mask);
}

static int ifs_online_cpu(unsigned int cpu)
{
	/* If the CPU is coming online for the first time*/
	if (per_cpu(ifs_state, cpu).first_time == 0)
		ifs_first_time(cpu);

	cpumask_clear_cpu(cpu, &(per_cpu(ifs_state, cpu).mask));

	per_cpu(ifs_state, cpu).scan_task = kthread_create_on_node(scan_test_worker, (void *)&cpu,
								   cpu_to_node(cpu), "ifsCpu/%u",
								   cpu);
	if (IS_ERR(per_cpu(ifs_state, cpu).scan_task)) {
		pr_err("scan_test_worker task create failed\n");
		return PTR_ERR(per_cpu(ifs_state, cpu).scan_task);
	}
	kthread_bind(per_cpu(ifs_state, cpu).scan_task, cpu);
	wake_up_process(per_cpu(ifs_state, cpu).scan_task);

	return 0;
}

static int ifs_offline_cpu(unsigned int cpu)
{
	struct task_struct *thread;

	thread = per_cpu(ifs_state, cpu).scan_task;
	per_cpu(ifs_state, cpu).scan_task = NULL;

	if (thread)
		kthread_stop(thread);

	return 0;
}

static int __init ifs_init(void)
{
	const struct x86_cpu_id *m;
	u64 ia32_core_caps;
	int ret;

	/* ifs capability check */
	m = x86_match_cpu(ifs_cpu_ids);
	if (!m)
		return -ENODEV;
	if (rdmsrl_safe(MSR_IA32_CORE_CAPS, &ia32_core_caps))
		return -ENODEV;
	if (!(ia32_core_caps & MSR_IA32_CORE_CAPS_INTEGRITY))
		return -ENODEV;

	ret = load_ifs_binary();
	if (ret) {
		pr_err("loading ifs binaries failed\n");
		return ret;
	}

	init_completion(&test_thread_done);
	ret = cpuhp_setup_state(CPUHP_AP_ONLINE_DYN, "x86/ifs:online",
				ifs_online_cpu, ifs_offline_cpu);

	if (ret < 0) {
		pr_err("cpuhp_setup_failed\n");
		return ret;
	}
	cpuhp_scan_state = ret;

	return 0;
}

static void __exit ifs_exit(void)
{
	struct task_struct *thread;
	int cpu;

	cpus_read_lock();
	for_each_online_cpu(cpu) {
		thread = per_cpu(ifs_state, cpu).scan_task;
		per_cpu(ifs_state, cpu).scan_task = NULL;
		if (thread)
			kthread_stop(thread);
	}
	cpus_read_unlock();
	cpuhp_remove_state(cpuhp_scan_state);

	pr_info("unloaded 'In-Field Scan' module\n");
}

MODULE_LICENSE("GPL");
MODULE_INFO(name, "ifs");
MODULE_DESCRIPTION("ifs");
module_init(ifs_init);
module_exit(ifs_exit);
