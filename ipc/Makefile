# SPDX-License-Identifier: GPL-2.0

#
# This file extends the upstream file `./ipc/Makefile` in the kernel sources,
# and is not meant to be merged upstream.
#

subdir-ccflags-y := -I$(src)/../include -DCONFIG_BUS1_TEST=1
obj-$(CONFIG_BUS1) += bus1/
