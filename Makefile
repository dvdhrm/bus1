#
# Out-of-tree Bus1 Module
# This makefile builds the out-of-tree Bus1 module and all complementary
# elements, including documentation provided alongside the module.
#
# This Makefile serves as entry-point for the different targets. It does not
# build anything on its own. See the sub-makefiles for details.
#

#
# Module Extension
# We support building multiple bus1 modules and loading them at the same time.
# By setting BUS1EXT=2, the built module is called `bus2.ko', and the global
# entry-points are adjusted as well. Use this to run tests against development
# versions, while using `bus1.ko' to run the host machine.
#
BUS1EXT			?= 1

#
# Requires Tools
# These are paths to tools we need for building. The $(SHELL) variable is also
# auto picked by Make to run builtins.
# If these tools are located somewhere else on your machine, you can adjust the
# paths through environment variables.
#
SHELL			:= /bin/bash
DEPMOD			?= depmod
MKDIR			?= mkdir
MKDIR_P			?= $(MKDIR) -p
RM			?= rm

#
# Miscellaneous
# Bunch of miscellaneous helpers that make the Makefile a lot simpler. All of
# them should be straightforward.
#
PWD			:= $(shell pwd)
BUILDDIR		:= $(PWD)/build
KCONFIGS		:= CONFIG_BUS1=m CONFIG_BUS1_TEST=y

#
# Kernel Sources
# We autodetect the running kernel version and use it to figure out the install
# path of the kernel sources. If you build against different versions, or want
# to adjust the path to the kernel sources, simply override the variables
# through environment variables.
#
KERNELVER		?= $(shell uname -r)
MODLIB			?= $(INSTALL_MOD_PATH)/lib/modules/$(KERNELVER)
KERNELDIR 		?= $(MODLIB)

#
# Default Target
# By default, build the out-of-tree module and everything that belongs into the
# same build.
#
all: module
.PHONY: all

#
# Build Directory
# We use a separate build directory (specify it via BUILDDIR=). In case it is
# non-existant, create it here.
#

$(BUILDDIR):
	$(MKDIR_P) "$@"

#
# Module Target
# The 'module' target builds the bus1 kernel module as out-of-tree module. We
# use the kbuild infrastructure directly, so you must have kernel-sources
# installed.
#
# Unfortunately, kbuild cannot build into a target output directory if you use
# out-of-tree builds. Hence, we have to build in-source-tree here.
#
module:
	@$(MAKE) -C $(KERNELDIR)/build \
		M=$(PWD)/ipc \
		modules \
		$(KCONFIGS)
.PHONY: module

#
# Test
# This builds the self-tests, as 'kselftest' does not provide any out-of-tree
# integration.
#
test:
#	@$(MAKE) -C tools/testing/selftests/bus1/ \
#		$(KCONFIGS)
.PHONY: test

#
# Sparse
# This runs sparse as part of the build process to try to detect any common
# errors in the kernel code.
#
sparse:
	@$(MAKE) -C $(KERNELDIR)/build \
		M=$(PWD)/ipc \
		C=2 CF="-D__CHECK_ENDIAN" \
		$(KCONFIGS)
.PHONY: check

#
# Clean
# Clean the entire source tree from build artifacts. Usually, we would build in
# separate output directories (and all our additions do this), but the kbuild
# infrastructure does not support this with out-of-tree modules. Hence, we
# build in-tree and provide this cleaner.
#
clean:
	@$(MAKE) -C $(KERNELDIR)/build \
		M=$(PWD)/ipc \
		clean \
		$(KCONFIGS)
	@$(RM) -Rf $(BUILDDIR)
.PHONY: clean

#
# Check
# Run kernel tests against the kernel module. Since we provide an out-of-tree
# module, we must first load it into the kernel. In most cases you should make
# use of the BUS1EXT= variable to load it as secondary module and not disrupt
# your running system.
#
# Run `make check` to invoke the entire test-suite. This needs root-privileges
# to load the module, hence `sudo` must be available and configured.
#

check-prepare: module
	@sudo sh -c 'dmesg -c > /dev/null'
	-sudo sh -c 'rmmod bus$(BUS1EXT) 2>/dev/null >/dev/null'
.PHONY: check-prepare

check: test check-prepare
	@sudo sh -c 'echo "bus$(BUS1EXT): initialize" >/dev/kmsg'
	sudo sh -c 'insmod ipc/bus1/bus$(BUS1EXT).ko'
	@sudo sh -c 'echo "bus$(BUS1EXT): ready" >/dev/kmsg'
#	@sh -c \
#		'@$(MAKE) -C tools/testing/selftests/bus1/ \
#			run-tests \
#			$(KCONFIGS) ; \
#			(R=$$? ; test $R -eq 0 || dmesg -H ; exit $$R)'
#	@sudo sh -c \
#		'$(MAKE) -C tools/testing/selftests/bus1/ \
#			run-tests \
#			$(KCONFIGS) ; \
#			(R=$$? ; test $R -eq 0 || dmesg -H ; exit $$R)'
	@sudo sh -c 'echo "bus$(BUS1EXT): deinitialize" >/dev/kmsg'
	sudo sh -c 'rmmod bus$(BUS1EXT)'
	@sudo sh -c 'echo "bus$(BUS1EXT): done" >/dev/kmsg'
	dmesg -H
.PHONY: check
