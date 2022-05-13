// SPDX-License-Identifier: GPL-2.0-only
/* Copyright(c) 2021 Intel Corporation.
 *
 * Author: Jithu Joseph <jithu.joseph@intel.com>
 */

#include <linux/firmware.h>
#include <linux/platform_device.h>

#include "ifs.h"
static const char *ifs_path = "intel/ifs/";

struct ifs_header {
	u32 header_ver;
	u32 blob_revision;
	u32 date;
	u32 processor_sig;
	u32 check_sum;
	u32 loader_rev;
	u32 processor_flags;
	u32 metadata_size;
	u32 total_size;
	u32 fusa_info;
	u64 reserved;
};

#define IFS_HEADER_SIZE	(sizeof(struct ifs_header))
static struct ifs_header *ifs_header_ptr;	/* pointer to the ifs image header */
static u64 ifs_hash_ptr;			/* Address of ifs metadata (hash) */

static const struct firmware *load_binary(const char *path)
{
	struct platform_device *ifs_pdev;
	const struct firmware *fw;
	int err;

	ifs_pdev = platform_device_register_simple("ifs", -1, NULL, 0);
	if (IS_ERR(ifs_pdev)) {
		pr_err("platform device register failed\n");
		return NULL;
	}
	err = request_firmware_direct(&fw, path, &ifs_pdev->dev);
	if (err) {
		pr_err("ifs file %s load failed\n", path);
		goto out;
	}

out:
	platform_device_unregister(ifs_pdev);

	return fw;
}

/*
 * Compare the image version whenever loading a new image.
 * Load the new image only if it is later or equal than the current version.
 */
static bool is_newer_binary(int current_loaded_version, struct ifs_header *new_image_ptr)
{
	return current_loaded_version <= new_image_ptr->blob_revision;
}

/*
 * Load ifs image. Before loading ifs module, the ifs image must be located
 * in /lib/firmware/intel/ifs and named as {family/model/stepping}.{testname}.
 */
int load_ifs_binary(void)
{
	const struct firmware *scan_fw;
	char scan_path[256];
	int ret;

	snprintf(scan_path, sizeof(scan_path), "%s%02x-%02x-%02x.scan", ifs_path,
		 boot_cpu_data.x86, boot_cpu_data.x86_model, boot_cpu_data.x86_stepping);

	scan_fw = load_binary(scan_path);
	if (!scan_fw)
		return -ENOENT;

	/* only reload new scan image for later version than currently loaded */
	if (!is_newer_binary(ifs_params.loaded_version, (struct ifs_header *)scan_fw->data)) {
		pr_warn("Refusing to load older binary");
		ret = -EINVAL;
		goto out;
	}

	ifs_header_ptr = (struct ifs_header *)scan_fw->data;
	ifs_hash_ptr = (u64)(ifs_header_ptr + 1);

	ret = 0;
out:
	release_firmware(scan_fw);

	return ret;
}
