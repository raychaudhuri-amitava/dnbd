/*
 * net.c - network stuff for DNBD 
 * Copyright (C) 2006 Thorsten Zitterell <thorsten@zitterell.de>
 */

#include <linux/errno.h>
#include <linux/slab.h>
#include <linux/random.h>

#include "net.h"

/* return pointer to server structure */
dnbd_server_t *dnbd_get_server(dnbd_servers_t * servers, int id)
{
	if ((0 < id) && (id <= SERVERS_MAX))
		return &servers->serverlist[id - 1];
	else
		return NULL;
}

/* add a new server */
int dnbd_set_serverid(dnbd_servers_t * servers, int id)
{
	int result = -EINVAL;

	dnbd_server_t *server;

	if (!(server = dnbd_get_server(servers, id)))
		goto out;

	switch (server->state) {
	case SERVER_INACTIVE:
		break;
	case SERVER_ACTIVE:
		result = -EEXIST;
		goto out;
	case SERVER_STALLED:
		server->state = SERVER_ACTIVE;
		result = 0;
		goto out;
	}

	server->state = SERVER_ACTIVE;
	server->id = id;
	server->srtt = servers->timeout_min;
	server->weight = 0;
	server->last_rx = jiffies;
	server->last_tx = jiffies;

	servers->count++;
	result = 0;
      out:
	return result;
}

/* return server according to their weights (= probability) */
int dnbd_next_server(dnbd_servers_t * servers)
{
	int i;
	char rnd;
	dnbd_server_t *server = NULL;
	int id = 0;
	int weightsum = 0;

	/* get random byte from kernel */
	get_random_bytes(&rnd, 1);
	
	for (i = 0; i < SERVERS_MAX; i++) {
		server = &servers->serverlist[i];
		if ((server->state == SERVER_ACTIVE)
		    && ((weightsum += server->weight) > (unsigned char) rnd)) {
			id = server->id;
			    break;
		    }
	}

	/* alternatively, use server with highest weight */
/*	for (i = 0; i < SERVERS_MAX; i++) {
		server = &servers->serverlist[i];
		if ((server->state == SERVER_ACTIVE)
		    && (server->weight > weight))
			id = server->id;
	}*/

	return id;
}

/* remove a server */
void dnbd_rem_servers(dnbd_servers_t * servers)
{
	if (!servers->serverlist)
		return;

	kfree(servers->serverlist);
	servers->serverlist = NULL;
}

/* remove all servers */
void dnbd_clean_servers(dnbd_servers_t * servers)
{
	int i;
	for (i = 0; i < SERVERS_MAX; i++) {
		servers->serverlist[i].state = 0;
	}

}

/* update round trip time of a server */
void dnbd_rtt_server(dnbd_servers_t * servers, int id, int rtt)
{
	dnbd_server_t *server;

	if (!(server = dnbd_get_server(servers, id)))
		goto out;

	if (rtt > servers->timeout_max)
		rtt = TIMEOUT_MAX;
	else if (rtt < servers->timeout_min)
		rtt = TIMEOUT_MIN;

	down(&servers->sema);
	server->srtt = ((SRTT_BETA * server->srtt
			 + (((SRTT_BETA_BASE - SRTT_BETA) * rtt) << SRTT_SHIFT))
			/ SRTT_BETA_BASE);
	up(&servers->sema);

      out:
	return;
}

/* recalculate server weights */
void dnbd_servers_weight(dnbd_servers_t * servers)
{
	int i;
	int num_servers = 0;
	long weightsum = 0;
	long prod = 0;
	long asrtt = 0;
	int srtt = 0;
	dnbd_server_t *server;

	/* 
	 * float arithmetics in kernel would be nice...
	 */
	down(&servers->sema);

	for (i = 0; i < SERVERS_MAX; i++) {
		server = &servers->serverlist[i];

		if (server->state == SERVER_ACTIVE) {
			if (server->last_tx >
			    server->last_rx + servers->timeout_stalled) {
				printk(KERN_ERR
				       "dnbd: disable server #%i\n",
				       i + 1);
				server->state = SERVER_STALLED;
				continue;
			}
			srtt = (server->srtt ? server->srtt : 1);
			weightsum += WEIGHT_FACTOR / srtt;
			asrtt += srtt;
			num_servers++;
		}
	}

	if (!num_servers)
		goto out;

	servers->asrtt = asrtt / num_servers;

	for (i = 0; i < SERVERS_MAX; i++) {
		server = &servers->serverlist[i];

		if (server->state == SERVER_ACTIVE) {
			srtt = (server->srtt ? server->srtt : 1);
			prod = srtt * weightsum;

			if (prod > 0)
				server->weight = WEIGHT_NORMAL * WEIGHT_FACTOR / prod;
			else
				server->weight = WEIGHT_NORMAL / num_servers;
		}
	}
      out:
	up(&servers->sema);

}

/* fill buffer with server statistics in human readable form for /proc */
int dnbd_show_servers(dnbd_servers_t * servers, void *buf, int size)
{
	int i, n = 0;
	dnbd_server_t *server;

	n += snprintf(buf + n, size - n,
		      " timeout_min: %i jiffies\n timeout_max: %i jiffies\n",
		      servers->timeout_min, servers->timeout_max);

	n += snprintf(buf + n, size - n, "Average SRTT: %i\n",
		      servers->asrtt >> SRTT_SHIFT);

	for (i = 0; i < SERVERS_MAX; i++) {
		server = &servers->serverlist[i];

		switch (server->state) {
		case SERVER_INACTIVE:
			continue;
		case SERVER_STALLED:
			n += snprintf(buf + n, size - n,
				      " id: %i (stalled)\n", server->id);
			continue;
		default:
			n += snprintf(buf + n, size - n, " id: %i\n",
				      server->id);
		}
		n += snprintf(buf + n, size - n,
			      " srtt: %i\n", server->srtt >> SRTT_SHIFT);
		n += snprintf(buf + n, size - n,
			      " weight: %i (of %i)\n", server->weight,WEIGHT_NORMAL);
	}

	return n;
}

/* initialize servers */
int dnbd_servers_init(dnbd_servers_t * servers)
{
	int i;

	spin_lock_init(&servers->lock);
	init_MUTEX(&servers->sema);

	if (!(servers->serverlist =
	      (dnbd_server_t *) kmalloc(SERVERS_MAX *
					sizeof(dnbd_server_t),
					GFP_KERNEL)))
		return -EINVAL;

	for (i = 0; i < SERVERS_MAX; i++) {
		servers->serverlist[i].state = 0;
	}

	servers->count = 0;
	servers->timeout_min = TIMEOUT_MIN;
	servers->timeout_max = TIMEOUT_MAX;
	servers->timeout_stalled = TIMEOUT_STALLED;
	return 0;
}
