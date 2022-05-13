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

/**
 * struct ifs_params - global ifs parameter for all cpus.
 * @loaded_version: stores the currently loaded ifs image version.
 * @valid_chunks: number of chunks which could be validated.
 */
struct ifs_params {
	int loaded_version;
	int valid_chunks;
};

int load_ifs_binary(void);
extern struct ifs_params ifs_params;
#endif
