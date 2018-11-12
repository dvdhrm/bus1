// SPDX-License-Identifier: GPL-2.0
#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt
#include <linux/cred.h>
#include <linux/err.h>
#include <linux/file.h>
#include <linux/fs.h>
#include <linux/kernel.h>
#include <linux/miscdevice.h>
#include <linux/poll.h>
#include <linux/uidgid.h>
#include <uapi/linux/bus1.h>
#include "uapi.h"
#include "util/acct.h"
#include "util/util.h"

static int b1_uapi_cdev_open(struct inode *inode, struct file *file)
{
	struct b1_uapi_cdev *cdev = container_of(file->private_data,
						 struct b1_uapi_cdev, misc);
	struct b1_acct_resource *res;
	struct b1_uapi_peer *uapi;
	int r;

	res = b1_acct_map(&cdev->acct, __kuid_val(current_euid()));
	if (IS_ERR(res)) {
		r = PTR_ERR(res);
		res = NULL;
		goto exit;
	}

	uapi = b1_uapi_new(res);
	if (IS_ERR(uapi)) {
		r = PTR_ERR(uapi);
		uapi = NULL;
		goto exit;
	}

	file->private_data = uapi;
	r = 0;

exit:
	b1_acct_resource_unref(res);
	return r;
}

static int b1_uapi_cdev_release(struct inode *inode, struct file *file)
{
	struct b1_uapi_peer *uapi = file->private_data;

	b1_uapi_finalize(uapi);
	file->private_data = b1_uapi_free(uapi);

	return 0;
}

static unsigned int b1_uapi_cdev_poll(struct file *file,
				      struct poll_table_struct *wait)
{
	struct b1_uapi_peer *uapi = file->private_data;

	poll_wait(file, b1_uapi_get_waitq(uapi), wait);

	return b1_uapi_poll(uapi);
}

static int b1_uapi_cdev_ioctl_pair(struct file *file,
				   struct b1_uapi_peer *uapi,
				   unsigned long arg)
{
	struct bus1_cmd_pair __user *u_cmd = (void __user *)arg;
	struct bus1_cmd_pair cmd;
	struct b1_uapi_peer *other;
	struct fd fd = {};
	int r;

	BUILD_BUG_ON(_IOC_SIZE(BUS1_CMD_PAIR) != sizeof(cmd));

	if (copy_from_user(&cmd, u_cmd, sizeof(cmd)))
		return -EFAULT;

	if (cmd.fd2 >= 0) {
		fd = fdget(cmd.fd2);
		if (!fd.file) {
			r = -EBADF;
			goto exit;
		}
		if (fd.file->f_op != file->f_op) {
			r = -EOPNOTSUPP;
			goto exit;
		}

		other = fd.file->private_data;
	} else {
		other = uapi;
	}

	r = b1_uapi_pair(uapi,
			 other,
			 cmd.flags,
			 &cmd.object_id,
			 &cmd.handle_id);
	if (r < 0)
		goto exit;

	if (copy_to_user(u_cmd, &cmd, sizeof(cmd))) {
		r = -EFAULT;
		goto exit;
	}

	r = 0;

exit:
	fdput(fd);
	return r;
}

static int b1_uapi_cdev_ioctl_send(struct b1_uapi_peer *uapi,
				   unsigned long arg)
{
	struct bus1_cmd_send __user *u_cmd = (void __user *)arg;
	struct bus1_cmd_send cmd;
	const struct bus1_message __user *u_message;
	struct bus1_message message;
	const u64 __user *u_destinations;
	const int __user *u_errors;

	BUILD_BUG_ON(_IOC_SIZE(BUS1_CMD_SEND) != sizeof(cmd));

	if (copy_from_user(&cmd, u_cmd, sizeof(cmd)))
		return -EFAULT;

	u_destinations = (void __user *)(unsigned long)cmd.ptr_destinations;
	u_errors = (void __user *)(unsigned long)cmd.ptr_errors;
	u_message = (void __user *)(unsigned long)cmd.ptr_message;

	if (unlikely(cmd.ptr_destinations != (u64)u_destinations ||
		     cmd.ptr_errors != (u64)u_errors ||
		     cmd.ptr_message != (u64)u_message))
		return -EFAULT;
	if (copy_from_user(&message, u_message, sizeof(message)))
		return -EFAULT;

	return b1_uapi_send(uapi, cmd.flags,
			    cmd.n_destinations, u_destinations, u_errors,
			    &message);
}

static int b1_uapi_cdev_ioctl_recv(struct b1_uapi_peer *uapi,
				   unsigned long arg)
{
	struct bus1_cmd_recv __user *u_cmd = (void __user *)arg;
	struct bus1_cmd_recv cmd;
	struct bus1_message __user *u_message;
	struct bus1_message message;
	int r;

	BUILD_BUG_ON(_IOC_SIZE(BUS1_CMD_RECV) != sizeof(cmd));

	if (copy_from_user(&cmd, u_cmd, sizeof(cmd)))
		return -EFAULT;

	u_message = (void __user *)(unsigned long)cmd.ptr_message;

	if (unlikely(cmd.ptr_message != (u64)u_message))
		return -EFAULT;
	if (copy_from_user(&message, u_message, sizeof(message)))
		return -EFAULT;

	r = b1_uapi_recv(uapi,
			 cmd.flags,
			 &cmd.destination,
			 &message);
	if (r < 0)
		return r;

	if (copy_to_user(u_message, &message, sizeof(message)) ||
	    copy_to_user(u_cmd, &cmd, sizeof(cmd)))
		return -EFAULT;

	return 0;
}

static int b1_uapi_cdev_ioctl_destroy(struct b1_uapi_peer *uapi,
				      unsigned long arg)
{
	struct bus1_cmd_destroy __user *u_cmd = (void __user *)arg;
	struct bus1_cmd_destroy cmd;
	const u64 __user *u_objects;

	BUILD_BUG_ON(_IOC_SIZE(BUS1_CMD_DESTROY) != sizeof(cmd));

	if (copy_from_user(&cmd, u_cmd, sizeof(cmd)))
		return -EFAULT;

	u_objects = (void __user *)(unsigned long)cmd.ptr_objects;

	if (unlikely(cmd.ptr_objects != (u64)u_objects))
		return -EFAULT;

	return b1_uapi_destroy(uapi, cmd.flags, cmd.n_objects, u_objects);
}

static int b1_uapi_cdev_ioctl_acquire(struct b1_uapi_peer *uapi,
				      unsigned long arg)
{
	struct bus1_cmd_acquire __user *u_cmd = (void __user *)arg;
	struct bus1_cmd_acquire cmd;
	const u64 __user *u_handles;

	BUILD_BUG_ON(_IOC_SIZE(BUS1_CMD_ACQUIRE) != sizeof(cmd));

	if (copy_from_user(&cmd, u_cmd, sizeof(cmd)))
		return -EFAULT;

	u_handles = (void __user *)(unsigned long)cmd.ptr_handles;

	if (unlikely(cmd.ptr_handles != (u64)u_handles))
		return -EFAULT;

	return b1_uapi_acquire(uapi, cmd.flags, cmd.n_handles, u_handles);
}

static int b1_uapi_cdev_ioctl_release(struct b1_uapi_peer *uapi,
				      unsigned long arg)
{
	struct bus1_cmd_release __user *u_cmd = (void __user *)arg;
	struct bus1_cmd_release cmd;
	const u64 __user *u_handles;

	BUILD_BUG_ON(_IOC_SIZE(BUS1_CMD_RELEASE) != sizeof(cmd));

	if (copy_from_user(&cmd, u_cmd, sizeof(cmd)))
		return -EFAULT;

	u_handles = (void __user *)(unsigned long)cmd.ptr_handles;

	if (unlikely(cmd.ptr_handles != (u64)u_handles))
		return -EFAULT;

	return b1_uapi_release(uapi, cmd.flags, cmd.n_handles, u_handles);
}

static long b1_uapi_cdev_ioctl(struct file *file,
			       unsigned int cmd,
			       unsigned long arg)
{
	struct b1_uapi_peer *uapi = file->private_data;
	int r;

	switch (cmd) {
	case BUS1_CMD_PAIR:
		r = b1_uapi_cdev_ioctl_pair(file, uapi, arg);
		break;
	case BUS1_CMD_SEND:
		r = b1_uapi_cdev_ioctl_send(uapi, arg);
		break;
	case BUS1_CMD_RECV:
		r = b1_uapi_cdev_ioctl_recv(uapi, arg);
		break;
	case BUS1_CMD_DESTROY:
		r = b1_uapi_cdev_ioctl_destroy(uapi, arg);
		break;
	case BUS1_CMD_ACQUIRE:
		r = b1_uapi_cdev_ioctl_acquire(uapi, arg);
		break;
	case BUS1_CMD_RELEASE:
		r = b1_uapi_cdev_ioctl_release(uapi, arg);
		break;
	default:
		r = -ENOTTY;
		break;
	}

	return r;
}

static const struct file_operations b1_uapi_cdev_fops = {
	.owner			= THIS_MODULE,
	.open			= b1_uapi_cdev_open,
	.release		= b1_uapi_cdev_release,
	.poll			= b1_uapi_cdev_poll,
	.llseek			= noop_llseek,
	.unlocked_ioctl		= b1_uapi_cdev_ioctl,
	.compat_ioctl		= b1_uapi_cdev_ioctl,
};

/**
 * b1_uapi_cdev_init() - initialize char-dev API
 *
 * This registers a new char-dev API and returns it to the caller. Once the
 * object is returned, it will be live and ready.
 *
 * Return: A pointer to the new device is returned, ERR_PTR on failure.
 */
struct b1_uapi_cdev *b1_uapi_cdev_new(void)
{
	struct b1_uapi_cdev *cdev;
	int r;

	cdev = kzalloc(sizeof(*cdev), GFP_KERNEL);
	if (!cdev)
		return ERR_PTR(-ENOMEM);

	b1_acct_init(&cdev->acct);

	cdev->misc = (struct miscdevice){
		.fops	= &b1_uapi_cdev_fops,
		.minor	= MISC_DYNAMIC_MINOR,
		.name	= KBUILD_MODNAME,
		.mode	= S_IRUGO | S_IWUGO,
	};

	r = misc_register(&cdev->misc);
	if (r < 0) {
		cdev->misc.fops = NULL;
		goto error;
	}

	return cdev;

error:
	b1_uapi_cdev_free(cdev);
	return ERR_PTR(r);
}

/**
 * b1_uapi_cdev_free() - destroy char-dev API
 * @cdev:		char-dev to operate on, or NULL
 *
 * This unregisters and frees a previously registered char-dev API. See
 * b1_uapi_cdev_new() for the counter-part to this.
 *
 * If you pass NULL, this is a no-op.
 *
 * Return: NULL is returned.
 */
struct b1_uapi_cdev *b1_uapi_cdev_free(struct b1_uapi_cdev *cdev)
{
	if (!cdev)
		return NULL;

	if (cdev->misc.fops)
		misc_deregister(&cdev->misc);
	b1_acct_deinit(&cdev->acct);
	kfree(cdev);

	return NULL;
}
