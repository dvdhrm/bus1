// SPDX-License-Identifier: GPL-2.0
#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt
#include <linux/compat.h>
#include <linux/err.h>
#include <linux/file.h>
#include <linux/fs.h>
#include <linux/kernel.h>
#include <linux/uaccess.h>
#include <linux/uio.h>
#include "util.h"

/**
 * b1_import_vecs() - import vectors from user
 * @vecsp:		kernel memory to store vecs, preallocated
 * @n_totalp:		output storage for sum of all vector lengths
 * @u_vecs:		user pointer for vectors
 * @n_vecs:		number of vectors to import
 *
 * This copies the given vectors from user memory into the preallocated kernel
 * buffer. Sanity checks are performed on the memory of the vector-array, the
 * memory pointed to by the vectors and on the overall size calculation.
 *
 * If the vectors were copied successfully, @n_totalp will contain the sum of
 * all vector-lengths.
 *
 * Unlike most other functions, this function might modify its output buffer
 * even if it fails. That is, @vecsp might contain garbage if this function
 * fails. This is done for performance reasons.
 *
 * Return: 0 on success, negative error code on failure.
 */
int b1_import_vecs(struct iovec *vecsp,
		   size_t *n_totalp,
		   const struct iovec __user *u_vecs,
		   size_t n_vecs)
{
	size_t i, n_total = 0;

	if (n_vecs > UIO_MAXIOV)
		return -EMSGSIZE;
	if (n_vecs == 0) {
		*n_totalp = 0;
		return 0;
	}

	if (IS_ENABLED(CONFIG_COMPAT) && in_compat_syscall()) {
		/*
		 * Compat types and macros are protected by CONFIG_COMPAT,
		 * rather than providing a fallback. We want compile-time
		 * coverage, so provide fallback types. The IS_ENABLED(COMPAT)
		 * condition guarantees this is collected by the dead-code
		 * elimination, anyway.
		 */
#if IS_ENABLED(CONFIG_COMPAT)
		const struct compat_iovec __user *c_vecs =
							(void __user *)u_vecs;
		compat_uptr_t v_base;
		compat_size_t v_len;
		compat_ssize_t v_slen;
#else
		const struct iovec __user *c_vecs = u_vecs;
		void __user *v_base;
		size_t v_len;
		ssize_t v_slen;
#endif
		void __user *v_ptr;

		if (unlikely(!access_ok(c_vecs, n_vecs * sizeof(*c_vecs))))
			return -EFAULT;

		for (i = 0; i < n_vecs; ++i) {
			if (unlikely(__get_user(v_base, &c_vecs[i].iov_base) ||
				     __get_user(v_len, &c_vecs[i].iov_len)))
				return -EFAULT;

#if IS_ENABLED(CONFIG_COMPAT)
			v_ptr = compat_ptr(v_base);
#else
			v_ptr = v_base;
#endif
			v_slen = v_len;

			if (unlikely(v_slen < 0))
				return -EMSGSIZE;
			if (unlikely(!access_ok(v_ptr, v_len)))
				return -EFAULT;
			if (unlikely(v_len > MAX_RW_COUNT - n_total))
				return -EMSGSIZE;

			vecsp[i].iov_base = v_ptr;
			vecsp[i].iov_len = v_len;
			n_total += v_len;
		}
	} else {
		void __user *v_base;
		size_t v_len;

		if (copy_from_user(vecsp, u_vecs, n_vecs * sizeof(*vecsp)))
			return -EFAULT;

		for (i = 0; i < n_vecs; ++i) {
			v_base = vecsp[i].iov_base;
			v_len = vecsp[i].iov_len;

			if (unlikely((ssize_t)v_len < 0))
				return -EMSGSIZE;
			if (unlikely(!access_ok(v_base, v_len)))
				return -EFAULT;
			if (unlikely(v_len > MAX_RW_COUNT - n_total))
				return -EMSGSIZE;

			n_total += v_len;
		}
	}

	*n_totalp = n_total;
	return 0;
}
