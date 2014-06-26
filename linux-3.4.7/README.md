NEON-Linux kernel interface
==============
Linux kernel modifications/extensions for NEON
--------------

The set of files modified/added to Linux-3.4.7 to facilitate NEON
are listed below. Almost all modifications are concentrated within
the ifdef macro "CONFIG_NEON_FACE" which can be enabled or disabled
at will. This keeps the NEON-kernel interface small, allows us to
load/unload the module at any time, and makes it possible to isolate
for development and debugging purposes.

```
linux-3.4.7
    ├── README.md
    |  This file
    ├── config.3.4.7-neon
    |   The Linux kernel configuration for which NEON has been tested.
    |   Many kernel features have been disabled to keep the comiple time
    |   down to a few minutes. Adjust to your own platform. If you enable
    |   SMP, be prepared to do some synchronization debugging.
    ├── arch
    │   └── x86
    │       ├── include
    │       │   └── asm
    │       │       └── pf_in.h
    |       |           This file has been duplicated from arch/x86/mm/pf_in.h
    │       └── mm
    │           ├── fault.c
    |           |   NEON relies on a modified page-fault handler that
    |           |   allows the OS to control how memory-mapped GPU registers
    |           |   are accessed. Entry point to special fault handling here.
    │           └── pf_in.c
    |               Exposing instruction decoding routines that we want to use in
    |               the event of a managed fault
    ├── drivers
    │   ├── gpu
    |   |   The interface of the OS kernel and NEON is declared in this directory
    |   |   and the files that appear below.
    |   |   Makefiles and kernel-configs are making it possible to enable/disable
    |   |   this feature at compile time
    │   │   ├── Makefile
    │   │   └── neon
    │   │       ├── Kconfig
    │   │       ├── Makefile
    │   │       └── neon_face.c
    │   └── video
    │       └── Kconfig
    ├── include
    │   ├── linux
    │   │   ├── mm.h
    |   |   |   Exposing some page-walking routines to use with NEON
    │   │   └── sched.h
    |   |       Added a few entries to the process struct to capture GPU-accessing
    |   |       applications and save NEON-related management information
    │   ├── neon
    │   │   └── neon_face.h
    |   |       The interface of the OS kernel and NEON is defined here.
    │   └── trace
    │       └── events
    │           └── neon.h
    |           LTTNG tracepoints for NEON
    ├── kernel
    │   ├── exit.c
    |   |   As an application accessing the GPU and managed by NEON exits, the NEON
    |   |   module is notified to properly run its own exit procedures.
    │   └── fork.c
    |       As the process struct is extended with NEON-specific entries, the
    |       fork routine is updated accordingly to initialize them
    ├── mm
    │ └── memory.c
          Memory-management and page-walking routines are exposed to use
          with NEON
```

Code in this directory is released in a fashion that agrees with the
Linux kernel, so (unless otherwise suggested in the respective files)
GPL v3.0 or the latest version of the GNU General Public License.
If you 'd rather apply changes yourself (rather than apply a patch),
you should be able to easily merge this set of files into the
mainline Linux kernel v3.4.7; you can grab a copy from the main
[kernel repository](http://ftp.kernel.org/pub/linux/kernel/v3.x/linux-3.4.7.tar.xz).
