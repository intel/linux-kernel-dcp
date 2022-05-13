/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright(c) 2021 Intel Corporation.
 *
 * Author: Jithu Joseph <jithu.joseph@intel.com>
 */

#ifndef _IFS_H_
#define _IFS_H_

#undef pr_fmt
#define pr_fmt(fmt) "ifs: " fmt

/* These bits are in the IA32_CORE_CAPABILITIES MSR */
#define MSR_IA32_CORE_CAPS_INTEGRITY_BIT	2
#define MSR_IA32_CORE_CAPS_INTEGRITY		BIT(MSR_IA32_CORE_CAPS_INTEGRITY_BIT)

#define MSR_COPY_SCAN_HASHES			0x000002c2
#define MSR_SCAN_HASHES_STATUS			0x000002c3
#define MSR_AUTHENTICATE_AND_COPY_CHUNK		0x000002c4
#define MSR_CHUNKS_AUTHENTICATION_STATUS	0x000002c5
#define MSR_ACTIVATE_SCAN			0x000002c6
#define MSR_SCAN_STATUS				0x000002c7
#define SCAN_TEST_PASS				0
#define SCAN_TEST_FAIL				1
#define SCAN_NOT_TESTED				2
#define SPINUNIT				100
#define THREAD_WAIT				5

/* MSR_SCAN_HASHES_STATUS bit fields */
union ifs_scan_hashes_status {
	u64	data;
	struct {
		u64	chunk_size	:16;
		u64	num_chunks	:8;
		u64	rsvd1		:8;
		u64	error_code	:8;
		u64	rsvd2		:11;
		u64	max_core_limit	:12;
		u64	valid		:1;
	};
};

/* MSR_CHUNKS_AUTH_STATUS bit fields */
union ifs_chunks_auth_status {
	u64	data;
	struct {
		u64	valid_chunks	:8;
		u64	total_chunks	:8;
		u64	rsvd1		:16;
		u64	error_code	:8;
		u64	rsvd2		:24;
	};
};

/* MSR_ACTIVATE_SCAN bit fields */
union ifs_scan {
	u64	data;
	struct {
		u64	start	:8;
		u64	stop	:8;
		u64	rsvd	:16;
		u64	delay	:31;
		u64	sigmce	:1;
	};
};

/* MSR_SCAN_STATUS bit fields */
union ifs_status {
	u64	data;
	struct {
		u64	chunk_num		:8;
		u64	chunk_stop_index	:8;
		u64	rsvd1			:16;
		u64	error_code		:8;
		u64	rsvd2			:22;
		u64	control_error		:1;
		u64	signature_error		:1;
	};
};

/*
 * ifs_status.error_code values after rdmsr(SCAN_STATUS)
 * 0x0: no error.
 * 0x1: scan did not start because all sibling threads did not join.
 * 0x2: scan did not start because interrupt occurred prior to threads rendezvous
 * 0x3: scan did not start because power management conditions are inadequate.
 * 0x4: scan did not start because non-valid chunks in range stop_index:start_index.
 * 0x5: scan did not start because of mismatches in arguments between sibling threads.
 * 0x6: scan did not start because core is not capable of performing scan currently.
 * 0x7: not assigned.
 * 0x8: scan did not start because of exceed number of concurrent CPUs attempt to run scan.
 * 0x9: interrupt occurred. Scan operation aborted prematurely. Not all chunks executed.
 * 0xFE: not all scan chunks were executed. Maximum forward progress retries exceeded.
 *	 This is a driver populated error-code as hardware returns success in this scenario.
 */
#define IFS_NO_ERROR				0x0
#define IFS_OTHER_THREAD_DID_NOT_JOIN		0x1
#define IFS_INTERRUPTED_BEFORE_RENDEZVOUS	0x2
#define IFS_POWER_MGMT_INADEQUATE_FOR_SCAN	0x3
#define IFS_INVALID_CHUNK_RANGE			0x4
#define IFS_MISMATCH_ARGUMENTS_BETWEEN_THREADS	0x5
#define IFS_CORE_NOT_CAPABLE_CURRENTLY		0x6
/* Code 0x7 not assigned */
#define IFS_EXCEED_NUMBER_OF_THREADS_CONCURRENT	0x8
#define IFS_INTERRUPTED_DURING_EXECUTION	0x9

#define IFS_SW_TIMEOUT				0xFD
#define IFS_SW_PARTIAL_COMPLETION		0xFE

/**
 * struct ifs_params - global ifs parameter for all cpus.
 * @loaded_version: stores the currently loaded ifs image version.
 * @valid_chunks: number of chunks which could be validated.
 * @fail_mask: stores the cpus which failed the scan.
 * @not_tested_mask: stores the cpus which have not been tested.
 */
struct ifs_params {
	int loaded_version;
	int valid_chunks;
	struct cpumask fail_mask;
	struct cpumask pass_mask;
	struct cpumask not_tested_mask;
};

/**
 * struct ifs_state - per-cpu ifs parameter.
 * @scan_task: scan_task for kthread to run scan test on each cpu.
 * @first_time: to track if cpu is coming online for the first time.
 * @status: it holds simple status pass/fail/untested.
 * @scan_details: opaque scan status code from h/w.
 * @scan_wq: kthread task wait queue.
 * @mask: triggering the test by setting the mask.
 */
struct ifs_state {
	struct task_struct *scan_task;
	int first_time;
	int status;
	u64 scan_details;
	wait_queue_head_t scan_wq;
	struct cpumask mask;
};

DECLARE_PER_CPU(struct ifs_state, ifs_state);

int load_ifs_binary(void);
extern struct ifs_params ifs_params;
extern atomic_t siblings_in;
extern atomic_t siblings_out;
extern struct completion test_thread_done;
extern int cpu_sibl_ct;
#endif
