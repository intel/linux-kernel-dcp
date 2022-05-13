// SPDX-License-Identifier: GPL-2.0-only
/* Copyright(c) 2021 Intel Corporation.
 *
 * Author: Jithu Joseph <jithu.joseph@intel.com>
 */

#include <linux/firmware.h>
#include <linux/platform_device.h>
#include <asm/microcode_intel.h>

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
static int ifs_sanity_check(void *mc)
{
	struct microcode_header_intel *mc_header = mc;
	unsigned long total_size, data_size;
	u32 sum, i;

	total_size = get_totalsize(mc_header);
	data_size = get_datasize(mc_header);

	if ((data_size + MC_HEADER_SIZE > total_size) || (total_size % sizeof(u32))) {
		pr_err("bad ifs data file size.\n");
		return -EINVAL;
	}

	if (mc_header->ldrver != 1 || mc_header->hdrver != 1) {
		pr_err("invalid/unknown ifs update format.\n");
		return -EINVAL;
	}

	sum = 0;
	i = total_size / sizeof(u32);
	while (i--)
		sum += ((u32 *)mc)[i];

	if (sum) {
		pr_err("bad ifs data checksum, aborting.\n");
		return -EINVAL;
	}

	return 0;
}

static bool find_ifs_matching_signature(struct ucode_cpu_info *uci, void *mc)
{
	struct microcode_header_intel *shdr;
	unsigned int mc_size;

	shdr = (struct microcode_header_intel *)mc;
	mc_size = get_totalsize(shdr);

	if (!mc_size || ifs_sanity_check(shdr) < 0) {
		pr_err("ifs sanity check failure\n");
		return false;
	}

	if (!cpu_signatures_match(uci->cpu_sig.sig, uci->cpu_sig.pf, shdr->sig, shdr->pf)) {
		pr_err("ifs signature, pf not matching\n");
		return false;
	}

	return true;
}

static bool ifs_image_sanity_check(void *data)
{
	struct ucode_cpu_info uci;

	collect_cpu_info_early(&uci);

	return find_ifs_matching_signature(&uci, data);
}

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

	if (!ifs_image_sanity_check((void *)fw->data)) {
		pr_err("ifs header sanity check failed\n");
		release_firmware(fw);
		fw = NULL;
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
