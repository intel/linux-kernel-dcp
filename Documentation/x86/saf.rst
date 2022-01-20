.. SPDX-License-Identifier: GPL-2.0

=============
Scan At Field
=============

Introduction
------------

Scan At Field (SAF) provides hardware hooks to perform core logic tests
and report failures for portions of silicon that lack error detection
capabilities, which will be available in some server SKUs starting with Sapphire
Rapids. It offers infrastructure to specific users such as cloud providers or
OEMs to schedule tests and find in-field failures due to aging in silicon that
might not necessarily be reported with normal machine checks.

SAF uses content provided by Intel that can be regarded as firmware. The image
is authored and protected by the same integrity mechanisms that protect Intel
microcode. The image is specific to a processor family, model and stepping.
SAF image carries the Processor Signature such as family/model/stepping similar
to what we find in the microcode header. The kernel will load the image that
matches processor signature. The SAF image is signed by Intel for scan test
chunk (a.k.a. "test pattern") integrity. The "signed" SAF image contains SHA256
hashes that will be used for test chunks authentication. The SAF image will be
distributed via github [1].

Scan test is self-contained. Once the scan test starts, it takes over the CPU
core completely and does not allow interaction with any other part of the system.
SAF requires both threads to be isolated when the test is running until the test
is done, or if configured an external interrupt will break the test in progress
and its reported to system software. When catastrophic events are signaled,
currently ongoing test is aborted.

The SAF sysfs provides the interface to run scan tests. The administrator can
initiate the test either a targeted core or all cores. When the all core test
is initiated, the kernel will step through one core at a time from core 0 executing
the test content from chunk 0 to the last chunk, then moves to the next core.
Once all cores have been tested once, the test is stopped waiting for another
command to start scan. For "targeted core" testing, the administrator can target
a specific core and run scan on it. The kernel will only test the target core
starting from start_chunk to stop_chunk as specified in the sysfs.

SAF Image
---------

The SAF image has an external header for software to select the right image,
hashes that hardware uses to authenticate the test chunk is correct, and the
test content itself. SAF image carries the Processor Signature such as
family/model/stepping similar to what we find in the microcode header. The kernel
will load the image that matches processor signature.

The SAF image file follows a similar naming convention to Intel CPU microcode
files. The file must be located in the firmware directory where the microcode
files are placed and named as {family/model/stepping}.scan as below:

/lib/firmware/intel/ift/saf/{ff-mm-ss}.scan

SAF Image Loading
-----------------

Before a core can be scanned, the SAF image needs to be staged to protected
memory. The required sequences are 1) load the scan hashes; 2) load the test
chunks. The CPU will check if the test chunks match the hashes, otherwise
failure is indicated to system software. If all chunks have been authenticated
successfully, the chunks will be copied to secured memory. The scan hash copy
and authentication need only be done at the first logical cpu on each socket.

The administrator can reload the SAF image at runtime. For example, when the new
SAF image is released, the administrator can overwrite the old image with the same
filename. The reload can be done via sysfs. echo 1 > /sys/devices/system/cpu/scan/reload

Triggering scan
---------------

The SAF module creates scan worker kthreads for all online logical cpus when
installed. The administrator can trigger scan test via sysfs. Once scan test is
triggered on a specific logical cpu, it wakes up the corresponding worker thread
and its sibling threads to execute the test. Once the scan test is done, the
threads go back to thread wait for next signal to start a new scan.

Before a scan test begins, data is flushed from the L1/L2 caches on the core. While
a scan test is in the process, the core is isolated from the L3 and memory subsystems.
In other words, scan test is confined to core perimeter and will have no effect
outside of core.

In a core, the scan engine is shared between siblings. When the scan test is
triggered on a core, all the siblings join the core before the scan execution.
The scan result is a core scoped MSR, which the system software reflects that via
/sys/devices/system/cpu/cpu#/scan/scan_result.

The SAF module offers two main test modes. First of all, it provides the interface
to test all cores automatically via sysfs. When all core testing is triggered, the kernel
will start testing from the first online core and proceed to test each core one
after another. In the all core test, each core is tested from chunk 0 to the last
chunk enumerated. /sys/devices/system/cpu/scan/num_chunks
The "all core test" can be triggered via sysfs as below.

echo 1 > /sys/devices/system/cpu/scan/run_test_all

On the other hand, the administrator can only target a specific core with specific
chunks. In this mode, the administrator needs to specify the chunk range before
the scan. The basic flow is as follows.

1. Set the chunk range in a target core.
   echo "<start_chunk>" > /sys/devices/system/cpu/cpu#/scan/start_chunk
   echo "<stop_chunk>" > /sys/devices/system/cpu/cpu#/scan/stop_chunk

2. The scan for a core is started by writing the scan_start file for any of the threads in
   the core.
   echo 1 > /sys/devices/system/cpu/cpu#/scan/scan_start

There is a limit on how many cores can execute a targeted test simultaneously. This is
available in /sys/devices/system/cpu/max_parallel_tests.

Activating scan itself is protected against multiple scan starts. The SAF module does
not allow another scan initiation in the same core until the current scan test is finished.
Once scan is done, the administrator can check the result via sysfs. Whether scan was
run via all core test or target core test, the scan result is stored per logical cpu.
/sys/devices/system/cpu/cpu#/scan/scan_result

Scan may be aborted by some reasons. Scan test will be aborted with certain circumstances
such as when interrupt occurred or cpu does not have enough power budget for SAF. In this
case, the kernel restart scan from the chunk where it stopped. Scan will also be aborted
when the test is failed. In this case, the test is immediately stopped without retry.

Tunable Parameters
------------------

This module accepts four tunable parameters. These could be provided at
load time or can be modified at runtime through sysfs.
(/sys/devices/system/cpu/scan/<parameter_name>). The parameters are as
described below.

1. core_delay: How many milliseconds must the kernel wait after a test is done before
	       triggering another core test.
2. noint: When set, system interrupts are not allowed to interrupt a scan.
3. trigger_mce: It tells SAF to trigger a MCE when the test fails.
4. thread_wait: The maximum time to allow microcode to wait for a core
		and its sibling joining for synchronization.

.. [#f1] https://github.com/keybase
