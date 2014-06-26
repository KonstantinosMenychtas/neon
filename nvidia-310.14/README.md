NEON-Nvidia interface
==============
Nvidia blob interface modifications/extensions for NEON
--------------

All source code in this directory is released in a fashion that
agrees with Nvidia binary driver license. Extensions and modifications
apply on the original, open source files of the Nvidia binary driver version
[310.14beta](http://www.nvidia.com/download/driverResults.aspx/50101).

Files in this directory are listed below.

```
nvidia-310.14
    ├── kernel
    │   ├── nv.c
    |   │   Main entry point to nvidia binary driver. Initialization,
    |   │   ioctls, etc are re-routed through NEON and used to mark GPU
    |   │   driver state-machine transitions and scheduling related events.
    │   ├── nv.h
    |   │   Exposing a hardware-probing function declaration
    │   ├── nv-mlock.c
    |   │   Same functionality as nv.c, for tracking pinned buffers
    │   ├── nv-mmap.c
    │   │    Same functionality as nv.c, for tracking buffer memory mapping
    │   └── os-interface.c
    │       Modifications primarily for debugging purposes
    ├── README.md
    │   This file
    └── README.txt
````
Nvidia's README file, included primarily to make lincesing clear

If you 'd rather apply changes yourself (rather than apply a patch),
you should be able to easily merge this set of files into the
Nvidia binary driver v310.14 (or, with some care, later).

NEON has been tested with the particular driver version suggested
(310.14beta), but since the interface to NEON is small, it should
be easy to port to a newer driver version. NEON has been tested
with three generations of Nvidia GPUs: Nvidia GTX670 (Kepler),
GTX275 and NVS295 (Tesla). It has been shortly (but not extensively)
tested with a GTX460 (Fermi). NEON should be "easy" to port to other
GPUs, but some machine-specific work wil be needed (e.g. to
find GPU hex identifiers). Porting to a different setup than what
has been tested, might need extra care, especially if the nvidia-driver
interface to the kernel has changed.

It is possible to entirely do away with source changes on
the Nvidia binary driver interface, by passing the responsibility
for identifying the "hooks" in this code back to the mainline kernel.
Since the only reason to do this is licensing implications, it has
not been a priority for NEON.

