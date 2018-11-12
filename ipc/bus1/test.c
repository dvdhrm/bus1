// SPDX-License-Identifier: GPL-2.0
#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt
#include <linux/kconfig.h>
#include <linux/kernel.h>
#include "test.h"
#include "util/util.h"

/**
 * b1_test_run() - run internal tests
 *
 * This runs all internal tests, if enabled via the CONFIG_BUS1_TEST
 * configuration option. These tests contain unit-tests of the kernel-internal
 * bus1 engine.
 */
void b1_test_run(void)
{
	if (!IS_ENABLED(CONFIG_BUS1_TEST))
		return;

	b1_test_distq();
}
