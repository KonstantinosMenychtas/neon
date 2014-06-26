#/*****************************************************************************/
#/*!
#  \author  Konstantinos Menychtas --- kmenycht@cs.rochester.edu
#  \version 0.3
#  \brief  "Neon" module for black-box GPU kernel-level management
#           Reference : http://www.ravnborg.org/kbuild/makefiles.html
#*/
#/*****************************************************************************/

# sudo insmod <module_name.ko>
MODULE_NAME             := neon
MODULE_OBJECT           := $(MODULE_NAME).ko
obj-m                   := $(MODULE_NAME).o
$(MODULE_NAME)-objs     := neon_mod.o neon_help.o \
			   neon_core.o neon_control.o neon_sys.o \
			   neon_track.o neon_sched.o \
			   neon_policy.o neon_fcfs.o \
			   neon_timeslice.o neon_sampling.o \
			   neon_ui.o

# max debug level 4, 3 necessary for trace-report
EXTRA_CFLAGS            := -I$(src) -DNEON_DEBUG_LEVEL_0
KBUILD_EXTRA_SYMBOLS    :=

# x86/mm
EXTRA_CFLAGS            += -I$(src)/../linux/arch/x86/mm
KBUILD_EXTRA_SYMBOLS    += $(src)/../linux/arch/x86/Module.symvers

EXTRA_CFLAGS            += -I$(src)/../nvidia/kernel
KBUILD_EXTRA_SYMBOLS    += $(src)/../nvidia/kernel/Module.symvers

# track interface accesses
EXTRA_CFLAGS   += -DNEON_LTTRACE
# EXTRA_CFLAGS   += -DNEON_TRACE_REPORT
# EXTRA_CFLAGS   += -DNEON_USE_TIMESLICE
# EXTRA_CFLAGS   += -DNEON_USE_SAMPLING

# Linux kernel source location
MODULE_MODLIB := /lib/modules/$(shell uname -r)
ifdef SYSSRC
  MODULE_SOURCES := $(SYSSRC)
  MODULE_HEADERS := $(MODULE_SOURCES)/include
else
  MODULE_UNAME   := $(shell uname -r)
  MODULE_SOURCES := $(shell test -d $(MODULE_MODLIB)/source && \
	echo $(MODULE_MODLIB)/source || \
	echo $(MODULE_MODLIB)/build)
  MODULE_HEADERS := $(MODULE_SOURCES)/include
endif

MODULE_OUTPUT := $(MODULE_SOURCES)
KBUILD_PARAMS :=

ifdef SYSOUT
 ifneq ($(SYSOUT), $(MODULE_SOURCES))
  MODULE_OUTPUT := $(SYSOUT)
  KBUILD_PARAMS := KBUILD_OUTPUT=$(MODULE_OUTPUT)
 endif
else
 ifeq ($(MODULE_SOURCES), $(MODULE_MODLIB)/source)
  MODULE_OUTPUT := $(MODULE_MODLIB)/build
  KBUILD_PARAMS := KBUILD_OUTPUT=$(MODULE_OUTPUT)
 endif
endif

CC ?= cc
HOST_CC ?= $(CC)
LD ?= ld

ifndef ARCH
ARCH := $(shell uname -m | sed -e 's/i.86/i386/')
endif

MODULE_UNAME ?= $(shell $(CONFTEST) get_uname)
MODULE_ROOT := /lib/modules/$(MODULE_UNAME)/kernel/lib
TOPDIR ?= $(MODULE_SOURCES)

EXTRA_CFLAGS += -DMODULE -Wall 
EXTRA_CFLAGS += -mcmodel=kernel -mno-red-zone -UDEBUG -U_DEBUG -DNDEBUG

KBUILD_PARAMS += KBUILD_VERBOSE=1
KBUILD_PARAMS += ARCH=$(ARCH)
KBUILD_PARAMS += -C $(MODULE_SOURCES)
KBUILD_PARAMS += M=$(PWD) # KBUILD_EXTMOD=$(PWD)/arch/x86

all:module tags
install:module-install

module:
	@echo "$(MODULE_NAME): calling KBUILD..."; \
		$(MAKE) "CC=$(CC)" $(KBUILD_PARAMS) modules; \
		echo "$(MODULE_NAME): left KBUILD."; \
		if ! [ -f $(MODULE_OBJECT) ]; \
		then \
			echo "$(MODULE_OBJECT) failed to build!"; \
		exit 1; \
		fi

module-install: module
	@mkdir -p $(MODULE_ROOT)/video; \
	install -m 0664 -o root -g root $(MODULE_OBJECT) $(MODULE_ROOT)/$(MODULE_NAME); \
	PATH="$(PATH):/bin:/sbin" depmod -ae;

package-install: module-install
	@PATH="$(PATH):/bin:/sbin" modprobe $(MODULE_NAME) && \
	echo "$(MODULE_OBJECT) installed successfully.";

tags:
	@etags *[ch]

clean:
	@rm -f *.o *.ko
	@rm -f *.mod.c .*.cmd *.cmd *.order *.symvers *.markers
	@rm -rf .tmp* .$(MODULE_NAME)*

tidy:
	@rm -f *~ \#* TAGS

clobber: clean tidy
