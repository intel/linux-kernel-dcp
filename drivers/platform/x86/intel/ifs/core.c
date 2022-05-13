// SPDX-License-Identifier: GPL-2.0-only
/* Copyright(c) 2021 Intel Corporation.
 *
 * Author: Jithu Joseph <jithu.joseph@intel.com>
 */

#include <linux/module.h>
#include <asm/cpu_device_id.h>

#include "ifs.h"
struct ifs_params ifs_params;

#define X86_MATCH(model)					\
	X86_MATCH_VENDOR_FAM_MODEL_FEATURE(INTEL, 6,		\
		INTEL_FAM6_##model, X86_FEATURE_CORE_CAPABILITIES, NULL)

static const struct x86_cpu_id ifs_cpu_ids[] __initconst = {
	X86_MATCH(SAPPHIRERAPIDS_X),
	{}
};

MODULE_DEVICE_TABLE(x86cpu, ifs_cpu_ids);

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

	return 0;
}

static void __exit ifs_exit(void)
{
	pr_info("unloaded 'In-Field Scan' module\n");
}

MODULE_LICENSE("GPL");
MODULE_INFO(name, "ifs");
MODULE_DESCRIPTION("ifs");
module_init(ifs_init);
module_exit(ifs_exit);
