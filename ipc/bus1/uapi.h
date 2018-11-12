// SPDX-License-Identifier: GPL-2.0
#ifndef __B1_UAPI_H
#define __B1_UAPI_H

/**
 * DOC: Bus1 User-Space API
 *
 * XXX
 */

#include <linux/fs.h>
#include <linux/kernel.h>
#include <linux/miscdevice.h>
#include <linux/mutex.h>
#include <linux/rbtree.h>
#include <uapi/linux/bus1.h>
#include "util/acct.h"

struct b1_uapi_cdev;
struct b1_uapi_handle;
struct b1_uapi_object;
struct b1_uapi_peer;

struct b1_uapi_handle {
	struct rb_node rb_peer;
	u64 id;
	u64 n_public;
	struct {
		struct b1_uapi_handle *next;
		u64 n;
	} op;
};

struct b1_uapi_object {
	struct rb_node rb_peer;
	u64 id;
	struct {
		struct b1_uapi_object *next;
	} op;
};

struct b1_uapi_peer {
	struct mutex lock;
	u64 id_allocator;
	struct rb_root map_objects;
	struct rb_root map_handles;
};

struct b1_uapi_cdev {
	struct b1_acct acct;
	struct miscdevice misc;
};

/* uapi handles */

void b1_uapi_handle_init(struct b1_uapi_handle *handle);
void b1_uapi_handle_deinit(struct b1_uapi_handle *handle);

/* uapi objects */

void b1_uapi_object_init(struct b1_uapi_object *object);
void b1_uapi_object_deinit(struct b1_uapi_object *object);

/* uapi peers */

void b1_uapi_peer_init(struct b1_uapi_peer *peer);
void b1_uapi_peer_deinit(struct b1_uapi_peer *peer);

struct b1_uapi_peer *b1_uapi_new(struct b1_acct_resource *res);
struct b1_uapi_peer *b1_uapi_free(struct b1_uapi_peer *peer);

wait_queue_head_t *b1_uapi_get_waitq(struct b1_uapi_peer *peer);
unsigned int b1_uapi_poll(struct b1_uapi_peer *peer);
void b1_uapi_finalize(struct b1_uapi_peer *peer);

int b1_uapi_pair(struct b1_uapi_peer *peer1,
		 struct b1_uapi_peer *peer2,
		 u64 flags,
		 u64 *object_idp,
		 u64 *handle_idp);
int b1_uapi_send(struct b1_uapi_peer *peer,
		 u64 flags,
		 u64 n_destinations,
		 const u64 __user *u_destinations,
		 const int __user *u_errors,
		 const struct bus1_message *message);
int b1_uapi_recv(struct b1_uapi_peer *peer,
		 u64 flags,
		 u64 *destinationp,
		 struct bus1_message *message);
int b1_uapi_destroy(struct b1_uapi_peer *peer,
		    u64 flags,
		    u64 n_objects,
		    const u64 __user *u_objects);
int b1_uapi_acquire(struct b1_uapi_peer *peer,
		    u64 flags,
		    u64 n_handles,
		    const u64 __user *u_handles);
int b1_uapi_release(struct b1_uapi_peer *peer,
		    u64 flags,
		    u64 n_handles,
		    const u64 __user *u_handles);

/* cdevs */

struct b1_uapi_cdev *b1_uapi_cdev_new(void);
struct b1_uapi_cdev *b1_uapi_cdev_free(struct b1_uapi_cdev *cdev);

#endif /* __B1_UAPI_H */
