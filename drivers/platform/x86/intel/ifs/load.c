// SPDX-License-Identifier: GPL-2.0-only
/* Copyright(c) 2021 Intel Corporation.
 *
 * Author: Jithu Joseph <jithu.joseph@intel.com>
 */

#include <linux/firmware.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <asm/microcode_intel.h>

#include "ifs.h"

static const char *ifs_path = "intel/ifs/";
static bool ifs_loading_error;	/* error occurred during ifs hashes/chunk authentication.*/

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
static u64 ifs_test_image_ptr;			/* 256B aligned address of test pattern */

static const char * const scan_hash_status[] = {
	"Reserved",
	"Attempt to copy scan hashes when copy already in progress",
	"Secure Memory not set up correctly",
	"FuSaInfo.ProgramID does not match or ff-mm-ss does not match",
	"Reserved",
	"Integrity check failed",
	"Scan test is in progress"
};

static const char * const scan_authentication_status[] = {
	"No error reported",
	"Attempt to authenticate a chunk which is already marked as authentic",
	"Chunk authentication error. The hash of chunk did not match expected value"
};

/*
 * To copy scan hashes and authenticate test chunks, the initiating cpu must point
 * to the EDX:EAX to the test image in linear address.
 * Run wrmsr(MSR_COPY_SCAN_HASHES) for scan hash copy and run wrmsr(MSR_AUTHENTICATE_AND_COPY_CHUNK)
 * for scan hash copy and test chunk authetication.
 */
static int copy_hashes_authenticate_chunks(void *arg)
{
	union ifs_scan_hashes_status hashes_status;
	union ifs_chunks_auth_status chunk_status;
	int i, num_chunks, chunk_size;
	bool hash_valid = false;
	u64 linear_addr, base;
	u32 err_code;

	/* run scan hash copy */
	wrmsrl(MSR_COPY_SCAN_HASHES, ifs_hash_ptr);
	rdmsrl(MSR_SCAN_HASHES_STATUS, hashes_status.data);

	/* enumerate the scan image information */
	num_chunks = hashes_status.num_chunks;
	chunk_size = hashes_status.chunk_size * 1024;
	hash_valid = hashes_status.valid;
	err_code = hashes_status.error_code;

	if (!hash_valid) {
		ifs_loading_error = true;
		if (err_code >= ARRAY_SIZE(scan_hash_status)) {
			pr_err("invalid error code 0x%x for hash copy\n", err_code);
			return -EINVAL;
		}
		pr_err("ifs: %s", scan_hash_status[err_code]);
		return -ENODEV;
	}
	pr_info("the total chunk number: %d\n", num_chunks);

	/* base linear address to the scan data */
	base = ifs_test_image_ptr;

	/* scan data authentication and copy chunks to secured memory */
	for (i = 0; i < num_chunks; i++) {
		linear_addr = base + i * chunk_size;
		linear_addr |= i;

		wrmsrl(MSR_AUTHENTICATE_AND_COPY_CHUNK, linear_addr);
		rdmsrl(MSR_CHUNKS_AUTHENTICATION_STATUS, chunk_status.data);

		ifs_params.valid_chunks = chunk_status.valid_chunks;
		err_code = chunk_status.error_code;

		if (err_code) {
			ifs_loading_error = true;
			if (err_code >= ARRAY_SIZE(scan_authentication_status)) {
				pr_err("invalid error code 0x%x for authentication\n", err_code);
				return -EINVAL;
			}
			pr_err("%s\n", scan_authentication_status[err_code]);
			return -ENODEV;
		}
	}

	return 0;
}

/*
 * IFS requires scan chunks authenticated per each socket in the platform.
 * Once the test chunk is authenticated, it is automatically copied to secured memory
 * and proceed the authentication for the next chunk.
 */
static int scan_chunks_sanity_check(void)
{
	int metadata_size, total_size, test_size, curr_pkg, cpu, ret = -ENOMEM;
	bool *package_authenticated;
	char *test_ptr;

	package_authenticated = kcalloc(topology_max_packages(), sizeof(bool), GFP_KERNEL);
	if (!package_authenticated)
		return ret;

	metadata_size = ifs_header_ptr->metadata_size;

	/* Spec says that if the Meta Data Size = 0 then it should be treated as 2000 */
	if (metadata_size == 0)
		metadata_size = 2000;

	/* Scan chunk start must be 256 byte aligned */
	if ((metadata_size + IFS_HEADER_SIZE) % 256) {
		pr_err("Scan pattern offset within the binary is not 256 byte aligned\n");
		return -EINVAL;
	}

	total_size = ifs_header_ptr->total_size;

	test_size = total_size - metadata_size - IFS_HEADER_SIZE;
	test_ptr = (char *)ifs_header_ptr + IFS_HEADER_SIZE + metadata_size;

	ifs_test_image_ptr = (u64)test_ptr;
	ifs_params.loaded_version = ifs_header_ptr->blob_revision;

	/* copy the scan hash and authenticate per package */
	cpus_read_lock();
	for_each_online_cpu(cpu) {
		curr_pkg = topology_physical_package_id(cpu);
		if (package_authenticated[curr_pkg])
			continue;
		package_authenticated[curr_pkg] = 1;
		ret = smp_call_function_single(cpu, (void *)copy_hashes_authenticate_chunks,
					       NULL, 1);
		if (ret || ifs_loading_error) {
			ret = ifs_loading_error ? -ENOMEM : ret;
			goto out;
		}
	}

out:
	cpus_read_unlock();
	kfree(package_authenticated);

	return ret;
}

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

	ret = scan_chunks_sanity_check();
out:
	release_firmware(scan_fw);

	return ret;
}
