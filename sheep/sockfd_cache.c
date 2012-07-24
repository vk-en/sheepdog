/*
 * Copyright (C) 2012 Taobao Inc.
 *
 * Liu Yuan <namei.unix@gmail.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License version
 * 2 as published by the Free Software Foundation.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 */

/*
 * The sockfd cache provides us long TCP connections connected to the nodes
 * in the cluster to accerlater the data transfer, which has the following
 * characteristics:
 *    0 dynamically allocated/deallocated at node granularity.
 *    1 cached fds are multiplexed by all threads.
 *    2 each session (for e.g, forward_write_obj_req) can grab one fd at a time.
 *    3 if there isn't any FD available from cache, use normal connect_to() and
 *      close() internally.
 *    4 FD are named by IP:PORT uniquely, hence no need of resetting at
 *      membership change.
 *    5 the total number of FDs is scalable to massive nodes.
 *    6 total 3 APIs: sheep_{get,put,del}_sockfd().
 */
#include <urcu/uatomic.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>

#include "sheep_priv.h"
#include "list.h"
#include "rbtree.h"
#include "logger.h"
#include "util.h"

struct sockfd_cache {
	struct rb_root root;
	pthread_rwlock_t lock;
	int count;
};

static struct sockfd_cache sockfd_cache = {
	.root = RB_ROOT,
	.lock = PTHREAD_RWLOCK_INITIALIZER,
};

struct sockfd_cache_entry {
	struct rb_node rb;
	struct node_id nid;
#define SOCKFD_CACHE_MAX_FD	8 /* How many FDs we cache for one node */
	/*
	 * FIXME: Make this macro configurable.
	 *
	 * Suppose request size from Guest is 512k, then 4M / 512k = 8, so at
	 * most 8 requests can be issued to the same sheep object. Based on this
	 * assumption, '8' would be effecient for servers that only host 4~8
	 * Guests, but for powerful servers that can host dozens of Guests, we
	 * might consider bigger value.
	 */
	int fd[SOCKFD_CACHE_MAX_FD];
	uint8_t fd_in_use[SOCKFD_CACHE_MAX_FD];
};

static struct sockfd_cache_entry *
sockfd_cache_insert(struct sockfd_cache_entry *new)
{
	struct rb_node **p = &sockfd_cache.root.rb_node;
	struct rb_node *parent = NULL;
	struct sockfd_cache_entry *entry;

	while (*p) {
		int cmp;

		parent = *p;
		entry = rb_entry(parent, struct sockfd_cache_entry, rb);
		cmp = node_id_cmp(&new->nid, &entry->nid);

		if (cmp < 0)
			p = &(*p)->rb_left;
		else if (cmp > 0)
			p = &(*p)->rb_right;
		else
			return entry;
	}
	rb_link_node(&new->rb, parent, p);
	rb_insert_color(&new->rb, &sockfd_cache.root);

	return NULL; /* insert successfully */
}

static struct sockfd_cache_entry *sockfd_cache_search(struct node_id *nid)
{
	struct rb_node *n = sockfd_cache.root.rb_node;
	struct sockfd_cache_entry *t;

	while (n) {
		int cmp;

		t = rb_entry(n, struct sockfd_cache_entry, rb);
		cmp = node_id_cmp(nid, &t->nid);

		if (cmp < 0)
			n = n->rb_left;
		else if (cmp > 0)
			n = n->rb_right;
		else
			return t; /* found it */
	}

	return NULL;
}

static inline int get_free_slot(struct sockfd_cache_entry *entry)
{
	int idx = -1, i;

	for (i = 0; i < SOCKFD_CACHE_MAX_FD; i++) {
		if (uatomic_cmpxchg(&entry->fd_in_use[i], 0, 1))
			continue;
		idx = i;
		break;
	}
	return idx;
}

/*
 * Grab a free slot of the node and inc the refcount of the slot
 *
 * If no free slot available, this typically means we should use short FD.
 */
static struct sockfd_cache_entry *sockfd_cache_grab(struct node_id *nid,
						    char *name, int *ret_idx)
{
	struct sockfd_cache_entry *entry;

	pthread_rwlock_rdlock(&sockfd_cache.lock);
	entry = sockfd_cache_search(nid);
	if (!entry) {
		dprintf("failed node %s:%d\n", name, nid->port);
		goto out;
	}

	*ret_idx = get_free_slot(entry);
	if (*ret_idx == -1)
		entry = NULL;
out:
	pthread_rwlock_unlock(&sockfd_cache.lock);
	return entry;
}

static inline bool slots_all_free(struct sockfd_cache_entry *entry)
{
	int i;
	for (i = 0; i < SOCKFD_CACHE_MAX_FD; i++)
		if (uatomic_read(&entry->fd_in_use[i]))
			return false;
	return true;
}

static inline void destroy_all_slots(struct sockfd_cache_entry *entry)
{
	int i;
	for (i = 0; i < SOCKFD_CACHE_MAX_FD; i++)
		if (entry->fd[i] != -1)
			close(entry->fd[i]);
}

/*
 * Destroy all the Cached FDs of the node
 *
 * We don't proceed if some other node grab one FD of the node. In this case,
 * the victim node will finally find itself talking to a dead node and call
 * sheep_del_fd() to delete this node from the cache.
 */
static bool sockfd_cache_destroy(struct node_id *nid)
{
	struct sockfd_cache_entry *entry;

	pthread_rwlock_wrlock(&sockfd_cache.lock);
	entry = sockfd_cache_search(nid);
	if (!entry) {
		dprintf("It is already destroyed\n");
		goto false_out;
	}

	if (!slots_all_free(entry)) {
		dprintf("Some victim still holds it\n");
		goto false_out;
	}

	rb_erase(&entry->rb, &sockfd_cache.root);
	pthread_rwlock_unlock(&sockfd_cache.lock);

	destroy_all_slots(entry);
	free(entry);

	return true;
false_out:
	pthread_rwlock_unlock(&sockfd_cache.lock);
	return false;
}

/* When node craches, we should delete it from the cache */
void sockfd_cache_del(struct node_id *nid)
{
	char name[INET6_ADDRSTRLEN];
	int n;

	if (!sockfd_cache_destroy(nid))
		return;

	n = uatomic_sub_return(&sockfd_cache.count, 1);
	addr_to_str(name, sizeof(name), nid->addr, 0);
	dprintf("%s:%d, count %d\n", name, nid->port, n);
}

static void sockfd_cache_add_nolock(struct node_id *nid)
{
	struct sockfd_cache_entry *new = xzalloc(sizeof(*new));
	int i;

	for (i = 0; i < SOCKFD_CACHE_MAX_FD; i++)
		new->fd[i] = -1;

	memcpy(&new->nid, nid, sizeof(struct node_id));
	if (sockfd_cache_insert(new)) {
		free(new);
		return;
	}
	sockfd_cache.count++;
}

/* Add group of nodes to the cache */
void sockfd_cache_add_group(struct sd_node *nodes, int nr)
{
	struct sd_node *p;

	dprintf("%d\n", nr);
	pthread_rwlock_wrlock(&sockfd_cache.lock);
	while (nr--) {
		p = nodes + nr;
		sockfd_cache_add_nolock(&p->nid);
	}
	pthread_rwlock_unlock(&sockfd_cache.lock);
}

/* Add one node to the cache means we can do caching tricks on this node */
void sockfd_cache_add(struct node_id *nid)
{
	struct sockfd_cache_entry *new = xzalloc(sizeof(*new));
	char name[INET6_ADDRSTRLEN];
	int n, i;

	for (i = 0; i < SOCKFD_CACHE_MAX_FD; i++)
		new->fd[i] = -1;

	memcpy(&new->nid, nid, sizeof(struct node_id));
	pthread_rwlock_rdlock(&sockfd_cache.lock);
	if (sockfd_cache_insert(new)) {
		free(new);
		pthread_rwlock_unlock(&sockfd_cache.lock);
		return;
	}
	pthread_rwlock_unlock(&sockfd_cache.lock);
	n = uatomic_add_return(&sockfd_cache.count, 1);
	addr_to_str(name, sizeof(name), nid->addr, 0);
	dprintf("%s:%d, count %d\n", name, nid->port, n);
}

static struct sockfd *sockfd_cache_get(struct node_id *nid, char *name)
{
	struct sockfd_cache_entry *entry;
	struct sockfd *sfd;
	int fd, idx;

	entry = sockfd_cache_grab(nid, name, &idx);
	if (!entry)
		return NULL;

	if (entry->fd[idx] != -1) {
		dprintf("%s:%d, idx %d\n", name, nid->port, idx);
		goto out;
	}

	/* Create a new cached connection for this vnode */
	dprintf("create connection %s:%d idx %d\n", name, nid->port, idx);
	fd = connect_to(name, nid->port);
	if (fd < 0) {
		uatomic_dec(&entry->fd_in_use[idx]);
		return NULL;
	}
	entry->fd[idx] = fd;

out:
	sfd = xmalloc(sizeof(*sfd));
	sfd->fd = entry->fd[idx];
	sfd->idx = idx;
	return sfd;
}

static void sockfd_cache_put(struct node_id *nid, int idx)
{
	struct sockfd_cache_entry *entry;
	char name[INET6_ADDRSTRLEN];
	int refcnt;

	addr_to_str(name, sizeof(name), nid->addr, 0);
	dprintf("%s:%d idx %d\n", name, nid->port, idx);

	pthread_rwlock_rdlock(&sockfd_cache.lock);
	entry = sockfd_cache_search(nid);
	pthread_rwlock_unlock(&sockfd_cache.lock);

	assert(entry);
	refcnt = uatomic_cmpxchg(&entry->fd_in_use[idx], 1, 0);
	assert(refcnt == 1);
}

/*
 * Return a sockfd connected to the vnode to the caller
 *
 * Try to get a 'long' FD as best, which is cached and never closed. If no FD
 * available, we return a 'short' FD which is supposed to be closed by
 * sheep_put_sockfd().
 *
 * ret_idx is opaque to the caller, -1 indicates it is a short FD.
 */
struct sockfd *sheep_get_sockfd(struct node_id *nid)
{
	char name[INET6_ADDRSTRLEN];
	struct sockfd *sfd;
	int fd;

	addr_to_str(name, sizeof(name), nid->addr, 0);
	sfd = sockfd_cache_get(nid, name);
	if (sfd)
		return sfd;

	/* Create a fd that is to be closed */
	fd = connect_to(name, nid->port);
	if (fd < 0) {
		dprintf("failed connect to %s:%d\n", name, nid->port);
		return NULL;
	}

	sfd = xmalloc(sizeof(*sfd));
	sfd->idx = -1;
	sfd->fd = fd;
	dprintf("%d\n", fd);
	return sfd;
}

/*
 * Rlease a sockfd connected to the vnode, which is acquired from
 * sheep_get_sockfd()
 *
 * If it is a long FD, just decrease the refcount to make it available again.
 * If it is a short FD, close it.
 *
 * sheep_put_sockfd() or sheep_del_sockfd() should be paired with
 * sheep_get_sockfd()
 */

void sheep_put_sockfd(struct node_id *nid, struct sockfd *sfd)
{
	if (sfd->idx == -1) {
		dprintf("%d\n", sfd->fd);
		close(sfd->fd);
		free(sfd);
		return;
	}

	sockfd_cache_put(nid, sfd->idx);
	free(sfd);
}

/*
 * Delete a sockfd connected to the vnode, when vnode is crashed.
 *
 * If it is a long FD, de-refcount it and tres to destroy all the cached FDs of
 * this vnode in the cache.
 * If it is a short FD, just close it.
 */
void sheep_del_sockfd(struct node_id *nid, struct sockfd *sfd)
{
	if (sfd->idx == -1) {
		dprintf("%d\n", sfd->fd);
		close(sfd->fd);
		free(sfd);
		return;
	}

	sockfd_cache_put(nid, sfd->idx);
	sockfd_cache_del(nid);
	free(sfd);
}
