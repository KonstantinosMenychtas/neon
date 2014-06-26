NEON
==============
A Disengaged Scheduling prototype for black-box GPUs
--------------

NEON is a thin software layer that sits between the GPU hardware and
the driver and enables the OS to control access to the GPU with minimal
overheads. The purpose of NEON is to enable fair, protected access to GPUs
or other, similar low-latency, high-performance devices that are mapped
directly to user-space - as is commonly the case for hardware accelerators.

NEON builds upon a scheduling technique dubbed "Disengaged Scheduling"
that enables the OS kernel to infrequently intercept and intercede on
interactions between the hardware and user-space processes. This OS kernel
engagement is "just enough" to allow the OS to monitor, account and control
access to the resource. At the same time, it is very efficient because the
OS kernel remains "disengaged" most of the time, allowing full, direct
access to the resource to select processes per some scheduling policy.

No changes to applications or libraries is required to use NEON.
It is implemented as a Linux kernel module, requiring only small, easily
update-able patches to the Linux kernel and the GPU's driver interface.
More precisely, NEON is composed of the following:
- a Linux kernel module
- an interface between the Linux kernel and the module
- an interface between the GPU driver and the module
- an unmodified GPU binary driver (blob)
It has been built and thoroughly tested on the Nvidia GTX670, GTX275 and
NVS295; it should be "easy" to port to other GPUs. It has been extensively
testesd using CUDA, OpenCL, OpenGL, WebGL, WebCL and similar APIs enabling
GPU acceleration. The implementation used and tested for these libraries
have been the latest supported by the Nvidia
[310.14beta](http://www.nvidia.com/download/driverResults.aspx/50101)
driver (e.g. CUDA 5.0, OpenGL 4.2 and OpenCL 1.1 at the time of this
writing). It should be "easy" to update and use newer drivers and libraries.
Similarly, though a unique hardware configuration has been tested
(Dell Precision T7500 Tower Workstation with 2.27 GHz Intel Xeon E5520
CPU and triple-channel 1.066 GHz DDR3 RAM)
there are little to no dependencies to the particular Linux kernel version
([3.4.7](http://ftp.kernel.org/pub/linux/kernel/v3.x/linux-3.4.7.tar.xz))
used in this implementation; updating to a newer kernel should be very
"easy".

NEON is released (primarily) as
[Free Software](http://www.fsf.org)
. More specifically :
- The NEON Linux kernel module is released under
[GPL](http://www.gnu.org/licenses/gpl.html)v3 or latest
- Any modifications to the Linux kernel to enable NEON are also released
under GPLv3 or latest
- Any modifications to the Nvidia blob kernel interface follow Nvidia's
binary drivers license
- Other software components of the NEON project (e.g. test benchmarks)
also follow GPLv3 or latest
All of the above hold for any piece of software contributed by the main
author (Konstantinos Menychtas) as well as future contributors to NEON.
Any 3rd party piece of code that appears in this repository with a license
that contradicts the above still follows the license intended by its
original authors (unless GPLv3 or later is compatible).

Other legalese :
- Trademarks, Copyrights and related material used in this document or this
repository belong to their respective holders
- The GPU driver used in this implementation is the one developed and released
by [Nvidia Corp](http://www.nvidia.com).
- NEON is not related to Nvidia; it has been developed entirely independently
and is primarily the product of clean-room reverse engineering.
Use at your own risk.
- NEON is unrelated to the [Nouveau](http://nouveau.freedesktop.org/wiki/)
project free software driver, or any other free software driver.
Documentation and source code by Nouveau have been loosely used for educational
purposes, but no code has been carried over.
- NEON is unrelated to the SIMD ARM extensions that go by a similar name,
or any other project that is not listed here.
- Tools developed by the free software community (primarily
[apitrace](https://github.com/apitrace/apitrace),
[valgrind-mmt](http://nouveau.freedesktop.org/wiki/Development/)
have been utilized in the reverse engineering effort required
to build NEON. Only
[mmio-trace](https://www.kernel.org/doc/Documentation/trace/mmiotrace.txt),
now in mainline kernel, is used in NEON.

NEON is the product of research conducted at the University of Rochester
and supported by various NSF/other grants.
Please see the following references for a non-comprehensive list:

- [Disengaged Scheduling for Fair, Protected Access to Fast Computational
Accelerators](http://www.cs.rochester.edu/~kmenycht/papers/asplos14-menychtas.pdf)
(ASPLOS'14)
- [Enabling OS Research by Inferring Interactions in the Black-Box
GPU Stack](http://www.cs.rochester.edu/~kmenycht/papers/atc13-menychtas.pdf)
(USENIX ATC'13)

If you use or modify this software, you are required to include these
references and respect the intention of the author to release it as
Free Software. Please support this effort by pushing any changes and
improvements you make back to this repository.

Last, please understand that despite the effort to build a robust, quality
framework, this software has been developed to serve primarily educational
and researh purposes. Additions that add to the stability or help with
usability are likely to be added. In the meantime, known bugs, unnecessary
or necessary TODOs, puzzling and perhaps inapprropriate comments and all
sorts of loose coding practices might appear in this repository.
The author is solely responsible for those. If you use this code,
take no offence and commit your improvements as you see fit.

CopyLeft, Konstnantinos Menychtas, 2014
