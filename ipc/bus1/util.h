#ifndef __BUS1_UTIL_H
#define __BUS1_UTIL_H

/*
 * Copyright (C) 2013-2016 Red Hat, Inc.
 *
 * bus1 is free software; you can redistribute it and/or modify it under
 * the terms of the GNU Lesser General Public License as published by the
 * Free Software Foundation; either version 2.1 of the License, or (at
 * your option) any later version.
 */

/**
 * Utilities
 *
 * Random untility functions that don't belong to a specific object. Some of
 * them are copies from internal kernel functions (which lack an export
 * annotation), some of them are variants of internal kernel functions, and
 * some of them are our own.
 */

#include <linux/err.h>
#include <linux/kernel.h>
#include <linux/uio.h>

/* tell gcc that PTR_ERR() always returns negative integers */
#define BUS1_ERR(_r) (WARN_ON((_r) >= 0) ? -EINVAL : (_r))

int bus1_import_vecs(struct iovec *out_vecs,
		     size_t *out_length,
		     const void __user *vecs,
		     size_t n_vecs);
struct file *bus1_import_fd(const u32 __user *user_fd);
struct file *bus1_clone_file(struct file *file);

#endif /* __BUS1_UTIL_H */
