NEON patches
==============
Patches for Linux kernel 3.4.7 and Nvidia binary driver 310.14beta
--------------

You can get a copy of the linux kernel source code to apply the
NEON patch from
[here](http://ftp.kernel.org/pub/linux/kernel/v3.x/linux-3.4.7.tar.xz).
Extract the source and rename the directory to "linux-3.4.7.orig/"
Assuming the patch and "linux-3.4.7.orig/" have a common basename
(i.e. they are members of the same directory), apply the patch
as follows :
patch -s -p0 < file.patch
and rename
"mv linux-3.4.7.orig/ linux". *
Update the configuration file and build as usual.

You can get a copy of the Nvidia binary driver v310.14beta to apply
the NEON patch from
[here](http://www.nvidia.com/download/driverResults.aspx/50101).
Extract the nvidia driver using the command line options
./NVIDIA-Linux-x86_64-310.14.run --extract-only
Rename extracted directory to  "NVIDIA-Linux-x86-310.14.orig/" and,
as previously, apply the nvidia driver patch and rename to "nvidia". *
Follow the Nvidia installer instructions to build from extracted
and patched driver.

* The makefile for the NEON module has dependencies that expect
these directory names to be properly resolved.
