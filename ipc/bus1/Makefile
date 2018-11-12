# SPDX-License-Identifier: GPL-2.0

#
# This builds the bus1 module. It is only sourced if CONFIG_BUS1 is enabled.
# All source files are combined into a single bus1.ko module.
#
# By setting BUS1EXT=X, the bus1 module will be built as busX.ko, and
# KBUILD_MODNAME=busX. This has the effect that all exported objects have
# different names than usual and you can run your test-infrastructure against
# the busX.ko, while running your system on bus1.ko.
#

BUS1EXT ?= 1

ccflags-y := -I$(src)

bus$(BUS1EXT)-y :=		\
	core.o			\
	custom.o		\
	main.o			\
	test.o			\
	test-distq.o		\
	uapi.o			\
	uapi-cdev.o		\
	util/acct.o		\
	util/distq.o		\
	util/flist.o		\
	util/util.o

obj-$(CONFIG_BUS1) += bus$(BUS1EXT).o
