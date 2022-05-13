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

/**
 * struct ifs_params - global ifs parameter for all cpus.
 * @loaded_version: stores the currently loaded ifs image version.
 */
struct ifs_params {
	int loaded_version;
};

int load_ifs_binary(void);
extern struct ifs_params ifs_params;
#endif
