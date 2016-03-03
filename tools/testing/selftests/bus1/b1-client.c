/*
 * Copyright (C) 2013-2016 Red Hat, Inc.
 *
 * bus1 is free software; you can redistribute it and/or modify it under
 * the terms of the GNU Lesser General Public License as published by the
 * Free Software Foundation; either version 2.1 of the License, or (at
 * your option) any later version.
 */

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <linux/bus1.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/uio.h>
#include <sys/mman.h>
#include <unistd.h>
#include "b1-client.h"

struct b1_client {
	int fd;
	const void *pool_map;
	size_t pool_size;
};

static int b1_client_new(struct b1_client **out)
{
	struct b1_client *client;

	assert(out);

	client = malloc(sizeof(*client));
	if (!client)
		return -ENOMEM;

	client->fd = -1;
	client->pool_map = NULL;
	client->pool_size = 0;

	*out = client;
	return 0;
}

static int b1_client_query(struct b1_client *client)
{
	struct bus1_cmd_connect cmd = {
		.flags = BUS1_CONNECT_FLAG_QUERY,
	};
	int r;

	r = b1_client_ioctl(client, BUS1_CMD_CONNECT, &cmd);
	if (r < 0)
		return r;

	client->pool_size = cmd.pool_size;

	return 0;
}

static int b1_client_mmap(struct b1_client *client)
{
	const void *map;
	int r;

	assert(client);

	map = mmap(NULL, client->pool_size, PROT_READ, MAP_SHARED,
		   client->fd, 0);
	if (map == MAP_FAILED)
		return -errno;

	assert(map != NULL);

	client->pool_map = map;

	return 0;
}

int b1_client_new_from_fd(struct b1_client **out, int fd)
{
	struct b1_client *client;
	int r;

	assert(out);
	assert(fd >= 0);

	r = b1_client_new(&client);
	if (r < 0)
		return r;

	client->fd = fd;

	r = b1_client_query(client);
	if (r >= 0) {
		r = b1_client_mmap(client);
		if (r < 0) {
			b1_client_free(client);
			return r;
		}
	} else if (r < 0 &&  r != -ENOTCONN && r != -ESHUTDOWN) {
		b1_client_free(client);
		return r;
	}

	*out = client;

	return 0;
}

int b1_client_new_from_path(struct b1_client **out, const char *path)
{
	int r, fd;

	assert(out);

	if (!path)
		path = "/dev/bus1";

	fd = open(path, O_RDWR | O_CLOEXEC | O_NOCTTY | O_NONBLOCK);
	if (fd < 0)
		return -errno;

	r = b1_client_new(out);
	if (r < 0) {
		close(fd);
		return r;
	}

	(*out)->fd = fd;

	return 0;
}

struct b1_client *b1_client_free(struct b1_client *client)
{
	if (!client)
		return client;

	if (client->pool_map)
		munmap((void *)client->pool_map, client->pool_size);
	if (client->fd >= 0)
		close(client->fd);
	free(client);

	return NULL;
}

int b1_client_ioctl(struct b1_client *client, unsigned int cmd, void *arg)
{
	int r;

	assert(client);

	r = ioctl(client->fd, cmd, arg);
	if (r < 0)
		return -errno;

	return r;
}

int b1_client_connect(struct b1_client *client, size_t pool_size)
{
	struct bus1_cmd_connect cmd = {
		.flags = BUS1_CONNECT_FLAG_CLIENT,
		.pool_size = pool_size,
	};
	int r;

	assert(pool_size);

	r = b1_client_ioctl(client, BUS1_CMD_CONNECT, &cmd);
	if (r < 0)
		return r;

	client->pool_size = pool_size;

	r = b1_client_mmap(client);
	if (r < 0) {
		b1_client_disconnect(client);
		return r;
	}

	return 0;
}

int b1_client_reset(struct b1_client *client)
{
	struct bus1_cmd_connect cmd = {
		.flags = BUS1_CONNECT_FLAG_RESET,
	};

	return b1_client_ioctl(client, BUS1_CMD_CONNECT, &cmd);
}

int b1_client_disconnect(struct b1_client *client)
{
	int r;

	assert(client);

	r = b1_client_ioctl(client, BUS1_CMD_DISCONNECT, NULL);
	if (r < 0)
		return r;

	if (client->pool_map) {
		munmap((void *)client->pool_map, client->pool_size);
		client->pool_map = NULL;
		client->pool_size = 0;
	}

	return 0;
}

int b1_client_send(struct b1_client *client,
		   uint64_t flags,
		   const uint64_t *dests,
		   size_t n_dests,
		   const struct iovec *vecs,
		   size_t n_vecs)
{
	struct bus1_cmd_send cmd = {
		.flags = flags,
		.ptr_destinations = (unsigned long)dests,
		.n_destinations = n_dests,
		.ptr_vecs = (unsigned long)vecs,
		.n_vecs = n_vecs,
	};

	return b1_client_ioctl(client, BUS1_CMD_SEND, &cmd);
}

static const void *b1_client_slice_from_offset(struct b1_client *client,
					       uint64_t offset)
{
	assert(client);

	if (!client->pool_map || offset == BUS1_OFFSET_INVALID ||
	    offset >= client->pool_size)
		return NULL;

	return client->pool_map + offset;
}

static uint64_t b1_client_slice_to_offset(struct b1_client *client,
					  const void *slice)
{
	const uint8_t *pool8, *slice8 = slice;

	assert(client);

	pool8 = client->pool_map;

	if (!pool8 || slice8 < pool8 || slice8 >= pool8 + client->pool_size)
		return (uint64_t) -1;

	return slice8 - pool8;
}

int b1_client_recv(struct b1_client *client,
		   uint64_t flags,
		   const void **slicep,
		   size_t *sizep)
{
	struct bus1_cmd_recv cmd = {
		.flags = flags,
		.msg_offset = BUS1_OFFSET_INVALID,
	};
	const uint8_t *slice;
	int r;

	r = b1_client_ioctl(client, BUS1_CMD_RECV, &cmd);
	if (r < 0)
		return r;

	/* Sending and receiving fds are currently unsupported by these
         * helpers */
	assert(!cmd.msg_fds);

	slice = b1_client_slice_from_offset(client, cmd.msg_offset);
	assert(slice || !cmd.msg_size);

	if (slicep) {
		*slicep = slice;
	} else {
		r = b1_client_slice_release(client, slice);
		if (r < 0)
			return r;
	}

	if (slicep)
		*slicep = (const void *)slice;

	if (sizep)
		*sizep = cmd.msg_size;

	return 0;
}

int b1_client_slice_release(struct b1_client *client, const void *slice)
{
	uint64_t offset;

	if (!slice)
		return 0;

	offset = b1_client_slice_to_offset(client, slice);

	return b1_client_ioctl(client, BUS1_CMD_SLICE_RELEASE, &offset);
}
