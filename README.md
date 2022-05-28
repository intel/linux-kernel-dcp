Purpose
=======
Provide Best Known Configuration (BKC) kernel for Sapphire Rapids (SPR)
customers.

The BKC kernel is based on v5.15. New SPR feautre patches which are not
in v5.15 are added to the BKC kernel.

The public SPR BKC kernel is hosted in https://github.com/intel/linux-kernel-dcp

WARNING this kernel contains technology preview code that is
subject to change once it goes upstream. This kernel is
strictly for hardware validation, not production. Applications
tested against this kernel may behave differently, or may not
operate at all once the code is finalized in the mainline kernel.
Use at your own risk.

Release History
===============

SPR-BKC-PC-v8.5
----------------
127. IAX Crypto fix (Tom Zanussi)

SPR-BKC-PC-v8.4
----------------
126. TDX fix (Chao Gao)

SPR-BKC-PC-v8.3
----------------
125. TDX fix (Yuan Yao)

SPR-BKC-PC-v8.2
----------------
124. TDX fix (Kuppuswamy Sathyanarayanan)

SPR-BKC-PC-v8.1
----------------
123. af_key fix (Haimin Zhang)
- 9a564bccb78a76740ea9d75a259942df8143d02c: add __GFP_ZERO flag for compose_sadb_supported in function pfkey_register

SPR-BKC-PC-v7.7
----------------
122. TSC fixes (Feng Tang)
- b50db7095fe002fa3e16605546cba66bf1b68a3e: x86/tsc: Disable clocksource watchdog for TSC on qualified platorms
- c86ff8c55b8ae68837b2fa59dc0c203907e9a15f: clocksource: Avoid accidental unstable marking of clocksources

SPR-BKC-PC-v7.6
----------------
121. Updated IFS (Jithu Joseph and Tony Luck)

SPR-BKC-PC-v7.5
----------------
120. One CVE fix (Mark Horn)

SPR-BKC-PC-v7.4
----------------
119. kvm: Refine codes to make ABI compatible (Yi Sun)

SPR-BKC-PC-v7.3
----------------
118. CVE fixes (Mark Horn)

SPR-BKC-PC-v7.2
----------------
117. Add SPR-BKC-PC-v6.config (Fenghua Yu)

SPR-BKC-PC-v7.1
----------------
116. Fix a few IOMMU issues (Jacob Pan)

SPR-BKC-PC-v6.13
----------------
115. vfio: mdev: idxd: fix vdcm_reset() for vidxd device (Dave Jiang)

SPR-BKC-PC-v6.12
----------------
114. SDSI fixes (David Box)

SPR-BKC-PC-v6.11
----------------
113. x86/tdx: Add vsock to TDX device filter's allow list (Alexander Shishkin)

SPR-BKC-PC-v6.10
----------------
112. Updated: Slow down split lockers (Tony Luck)

SPR-BKC-PC-v6.9
----------------
111. iommu/vtd-: Fix regression issue in intel_svm_free_async_fn (Yi Liu)

SPR-BKC-PC-v6.8
----------------
110. Enhance intel-idle C states (Artem Bityutskiy)

SPR-BKC-PC-v6.7
----------------
109. Two IOMMU VT-D fixes (Yi Liu)

SPR-BKC-PC-v6.6
----------------
108. vfio: mdev: idxd: fix soft reset for guest (Dave Jiang)

SPR-BKC-PC-v6.5
----------------
106. X86: FPU: Matches init_fpstate::xcomp_bv with default allocated xsave components (Yuan Yao)
107. vfio: mdev: idxd: fix mm ref leak (Dave Jiang)

SPR-BKC-PC-v6.4
----------------
105. TDX attestation fix (Chenyi Qiang)

SPR-BKC-PC-v6.3
----------------
103. IDXD fixes (Dave Jiang)

SPR-BKC-PC-v6.2
----------------
102. x86/mm: Forbid the zero page once it has uncorrectable errors (Qiuxu Zhuo)

SPR-BKC-PC-v6.1
----------------
101. iommu/vt-d: Make PRQ size and allocation dynamic (Jacob Pan)

SPR-BKC-PC-v5.12
----------------
100. One IDXD fix (Dave Jiang) and one TDX performance fix (Chao Gao)

SPR-BKC-PC-v5.11
----------------
99. IDXD fix (Dave Jiang)

SPR-BKC-PC-v5.10
----------------
98. CVE fixes (Mark Horn)

SPR-BKC-PC-v5.9
----------------
97. One more DSA fix (Dave Jiang)

SPR-BKC-PC-v5.8
----------------
96. DSA fixes (Dave Jiang)

SPR-BKC-PC-v5.7
----------------
95. ifs: Remove version check during IFS binary load (Jithu Joseph)

SPR-BKC-PC-v5.6
----------------
94. TDX guest fixes (Kuppuswamy Sathyanarayanan)

SPR-BKC-PC-v5.5
----------------
93. Perf: Read call chain from CET shadow stack (Kan Liang)

SPR-BKC-PC-v5.4
----------------
92. Load a new TDX module to overwrite old one (Xiaoyao Li)

SPR-BKC-PC-v5.3
----------------
91. Fix TLB flushing issue for TDX guest's private pages (Yuan Yao)

SPR-BKC-PC-v5.2
----------------
90. Minor TDX/KVM/SGX/IOMMU fixes (Chenyi Qiang and Jacob Pan)

SPR-BKC-PC-v5.1
----------------
89. Fix a DSA issue (Dave Jiang)

SPR-BKC-PC-v4.24
----------------
88. Back port a few CVE commits from upstream (Mark Horn)

SPR-BKC-PC-v4.23
----------------
87. Fix DMA-API debug warning print when using DMA to do page migration (Jiangbo Wu)

SPR-BKC-PC-v4.22
----------------
86. kexec: Fix kexec with CET (Rick Edgecombe)

SPR-BKC-PC-v4.21
----------------
85. Fix DSA/IAX security issues (Dave Jiang)

SPR-BKC-PC-v4.20
----------------
84. Fix dmatest failed in guest with IAX passthrough (Dave Jiang)

SPR-BKC-PC-v4.19
----------------
83. KVM: TDX: Do TLB flushing for TD guest before drop the private page (Yuan Y
ao)

SPR-BKC-PC-v4.18
----------------
82. Fix DSA live migration (Yi Sun)

SPR-BKC-PC-v4.17
----------------
81. KVM: TDX: Enable CET in TD VM (Chenyi Qiang)

SPR-BKC-PC-v4.16
----------------
80. Fix TDX guest (Kuppuswamy Sathyanarayanan)

SPR-BKC-PC-v4.15
----------------
79. Fix scheduler fairness issue (Tim Chen and PeterZ)

SPR-BKC-PC-v4.14
----------------
78. Updated PKS (Ira Weiny)

SPR-BKC-PC-v4.13
----------------
77. DSA fix (Dave Jiang)

SPR-BKC-PC-v4.12
----------------
76. DMA pages copy support for page migration (Peter Zhu and Jiangbo Wu)

SPR-BKC-PC-v4.11
----------------
75. X86: TDX: Fix hang/panic when loading np_seamldr with shadow stack (CET) en
abled (Yuan Yao)

SPR-BKC-PC-v4.10
----------------
74. Add new IDXD and Zswap features:
- crypto: Enable iax_crypto_enable and iax_crypto_disable (Tom Zanussi)
- dmaengine: idxd: add wq driver name support for accel-config user tool (Dave 
Jiang)

SPR-BKC-PC-v4.9
----------------
73. Fix DSA/IAX issues (Dave Jiang)

SPR-BKC-PC-v4.8
----------------
72. Enable CET KVM (Weijiang Yang)

SPR-BKC-PC-v4.7
----------------
71. Fix TDX KVM TLB flushing bug (Yuan Yao)

SPR-BKC-PC-v4.6
----------------
70. Enable AMX feature in the TD VM (Yang Zhong)

SPR-BKC-PC-v4.5
----------------
69. Fix TDX to accommodate the BIOS DXE driver loader (Xiaoyao Li)

SPR-BKC-PC-v4.4
----------------
68. CET (Rick Edgecombe)

SPR-BKC-PC-v4.3
----------------
67. Updated IFS (Jithu Joseph)

SPR-BKC-PC-v4.1
----------------
66. Update TDX guest (Kirill Shutemov)

SPR-BKC-PC-v3.21
----------------
65. Fix https://nvd.nist.gov/vuln/detail/CVE-2022-23222 (Mark Horn and Miguel Bernal Marin):
- https://git.kernel.org/pub/scm/linux/kernel/git/stable/linux.git/commit/?h=linux-5.10.y&id=35ab8c9085b0af847df7fac9571ccd26d9f0f513
- https://git.kernel.org/pub/scm/linux/kernel/git/torvalds/linux.git/commit/?id=e60b0d12a95dcf16a63225cead4541567f5cb517
- https://git.kernel.org/pub/scm/linux/kernel/git/torvalds/linux.git/commit/?id=ca796fe66f7fceff17679ee6cc5fe4b4023de44d

64. Add SPR-BKC-PC-v3.config (Jair Gonzalez)

SPR-BKC-PC-v3.20
----------------
63. Fix vIOMMU GIOVA by avoiding reserved IOASIDs (Jacob Pan)

PR-BKC-PC-v3.19
----------------
62. Remove POC dynamic MSIX allocation code (Dave Jiang)

SPR-BKC-PC-v3.18
----------------
61. Back port some high/critical Common Vulnerabilities and Exposures (CVE) fixes from upstream (Makr Horn):
- commit: ec6af094ea28f0f2dda1a6a33b14cd57e36a9755
- commit: f9d87929d451d3e649699d0f1d74f71f77ad38f5
- commit: dfd0743f1d9ea76931510ed150334d571fbab49d
- commit: 83912d6d55be10d65b5268d1871168b9ebe1ec4b
- commit: 054aa8d439b9185d4f5eb9a90282d1ce74772969

SPR-BKC-PC-v3.17
----------------
60. Fix WQ config fails with sm_off (Dave Jiang)

59. Revert IOASID range adjustment (Jacob Pan)

SPR-BKC-PC-v3.16
----------------
58. Fix ZSWAP breakage (Tom Zanussi)

57. Rollback microcode (Ashok Raj)

56. Fix TDX seamcall (Isaku Yamahata)

SPR-BKC-PC-v3.15
----------------
55. Fix a NULL domain issue in IOMMU (Jacob Pan):

SPR-BKC-PC-v3.14
----------------
54. Check on PT SRE support on stepping (Jacob Pan)

SPR-BKC-PC-v3.13
----------------
53. Enable PASID for DMA API users (Jacob Pan)

SPR-BKC-PC-v3.12
----------------
52. sched/fair: Force progress on min_vruntime (Tim Chen)

SPR-BKC-PC-v3.11
----------------
51. dmaengine: idxd: restore traffic class defaults after wq reset (Dave Jiang)

SPR-BKC-PC-v3.10
----------------
50. Fix VMD booting issue (Adrian Huang <ahuang12@lenovo.com>):
- 5.16 commit: 2565e5b69c44b4e42469afea3cc5a97e74d1ed45

SPR-BKC-PC-v3.9
----------------
49. Sequential split lock (Tony Luck):

SPR-BKC-PC-v3.8
----------------
48. SST HFI (Pandruvada, Srinivas)

SPR-BKC-PC-v3.7
----------------
47. More IDXD fixes (Dave Jiang)

SPR-BKC-PC-v3.6
----------------
46. Update AMX KVM to upstream version (tip for 5.17) (Yang Zhong)

SPR-BKC-PC-v3.5
----------------
45. IDXD fix (Dave Jiang):

SPR-BKC-PC-v3.4
----------------
44. TDX: fix nosmap booting issue (Kirill Shutemov)

SPR-BKC-PC-v3.3
----------------
43. x86/microcode: adjust sequence of controlling KVM guest and EPC (Cathy Zhang)

SPR-BKC-PC-v3.2
----------------
42. Fix TDX issue (Chenyi Qiang)

41. Arch-lbr (Weijiang Yang)

40. Fix TDX issue (Kirill Shutemov)

SPR-BKC-PC-v3.1
----------------
39.  iommu/vt-d: Fix PCI bus rescan device hot add (Jacob Pan)

SPR-BKC-PC-v2.10
----------------
38. Enable MDEV and VFIO (Dave Jiang and Yi Liu)

PR-BKC-PC-v2.9
----------------
37. Fix TDX issues (Chenyi Qiang)

SPR-BKC-PC-v2.8
----------------
36. Add SPR-BKC-PC-v2.config (Miguel Bernal Martin)

SPR-BKC-PC-v2.7
----------------
35. Prevent SGX reclaimer from running during SGX SVN update (Cathy Zhang):

SPR-BKC-PC-v2.6
----------------
34. Perf Inject fix itrace space allowed for new attributes (Adrian Hunter)

SPR-BKC-PC-v2.5
----------------
33. firmware updates/telemetry support (Chen, Yu C)

SPR-BKC-PC-v2.4
---------------
31. VM Preserving Run-time Fixes (Chao Gao)

32. TDVMCALL[GetQuote] driver (Chenyi Qiang)

SPR-BKC-PC-v2.3
----------------
30. IAX Crypto (Dave Jiang)

SPR-BKC-PC-v2.2
----------------
28. SGX EDMM ( Reinette Chatre):

29. Updated SGX Seamless (old one reverted) (Cathy Zhang):

SPR-BKC-PC-v2.1
---------------
27. AMX fixes (Chang Seok Bae)

26. TDX fixes (Chao Gao)

25. VM Preserving Run-time (Chao Gao)

24. TDX Guest fixes (Kirill)

SPR-BKC-PC-v1.23
----------------
23. SPR-BKC-PC-v1.config (Gonzalez Plascencia, Jair De Jesus)
22. Seamless (Yu Chen)

SPR-BKC-PC-v1.22
----------------
21. SPR BKC PC kernel banne (Fenghua Yu)

SPR-BKC-PC-v1.21
----------------
20. KVM TDP/TDX fixes (Chao Gao)

SPR-BKC-PC-v1.20
----------------
19. SPI-NOR (Mika Westerberg)

SPR-BKC-PC-v1.19
----------------
18. AMX-KVM (Jing Liu)

SPR-BKC-PC-v1.18
----------------
17. SDSI (Dave Box)

SPR-BKC-PC-v1.17
----------------
16. SGX Seamless fix (Cathy Zhang):

SPR-BKC-PC-v1.16
----------------
15. SAF (Jithu Joseph and Kyung Min Park):

SPR-BKC-PC-v1.15
----------------
14. SEAM TDX bug fix (Chenyi Qiang)

SPR-BKC-PC-v1.14
----------------
13. SIOV (Jacob Pan)

SPR-BKC-PC-v1.13
----------------
12. DSA/IAX (Dave Jiang):

SPR-BKC-PC-v1.12
----------------

Linux kernel 5.15+tip/x86/fpu+following features.

tip/x86/fpu top commit: d7a9590f608d
    Documentation/x86: Add documentation for using dynamic XSTATE features

1. AMX (Chang Seok Bae)
2. ENQCMD and PASID (Fenghua Yu)
3. PKS (Ira Weiny)
4. TDX for guest (Kuppuswamy Sathyanarayanan)
5. SGX Reset (Yang Zhong)
6. KVM Interrupt (Guang Zeng)
7. SGX Seamless (Cathy Zhang)
8. PKS-KVM and Notify VM Exit (Chenyi Qiang)
9. HFI (Ricardo Neri and Srinivas Pandruvada)
10. TDX for host (Yuan Yao)
11. Fixes for TDX host and KVM (Yuan Yao)
