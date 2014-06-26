NEON module
==============
Core implementation of NEON as an autonomous kernel module
--------------

The NEON module builds upon an uncovered GPU driver
state machine that marks scheduling-related events and
implements different scheduling policies to manage access
to the GPU at the OS kernel level.

```
neon_module
    ├── Makefile
    ├── neon_control.c
    ├── neon_control.h
    ├── neon_core.c
    ├── neon_core.h
    ├── neon_fcfs.c
    ├── neon_fcfs.h
    ├── neon_help.c
    ├── neon_help.h
    ├── neon_mod.c
    ├── neon_policy.c
    ├── neon_policy.h
    ├── neon_sampling.c
    ├── neon_sampling.h
    ├── neon_sched.c
    ├── neon_sched.h
    ├── neon_sys.c
    ├── neon_sys.h
    ├── neon_timeslice.c
    ├── neon_timeslice.h
    ├── neon_track.c
    ├── neon_track.h
    ├── neon_ui.c
    └── neon_ui.h
```
