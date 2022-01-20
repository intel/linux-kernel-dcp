/* SPDX-License-Identifier: GPL-2.0 */
/* helper functions to invoke SEAM ACM. */

#ifndef _X86_TDX_SEAM_H
#define _X86_TDX_SEAM_H

#include <linux/earlycpio.h>

#include <asm/vmx.h>

bool __init seam_get_firmware(struct cpio_data *blob, const char *name);

int __init seam_init_vmx_early(void);
void __init seam_init_vmxon_vmcs(struct vmcs *vmcs);

int __init seam_vmxon_on_each_cpu(void);
int __init seam_vmxoff_on_each_cpu(void);

#endif /* _X86_TDX_SEAM_H */
