/*
 * net.c - network stuff for the server
 * Copyright (C) 2006 Thorsten Zitterell <thorsten@zitterell.de>
 */

#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <pthread.h>

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>

#define DNBD_USERSPACE		1
#include "../common/dnbd-cliserv.h"

#include "net.h"

struct listener_s {
	pthread_t tid;
	net_request_t *request;
};

typedef struct listener_s listener_t;
listener_t listener;

/* 
 * function net_tx(): send a server reply 
 */
void net_tx(net_info_t * net_info, net_reply_t * reply)
{
	if (sendto
	    (net_info->sock, reply->data, reply->len, 0,
	     (struct sockaddr *) &net_info->groupnet,
	     sizeof(net_info->groupnet)) < 0)
		fprintf(stderr, "net_tx: mcast sendproblem\n");

}

/* 
 * function net_rx(): receive a client request 
 * returns: 1 on correct size of reply, otherwise 0
 */
int net_rx(net_info_t * net_info, net_request_t * request)
{
	ssize_t n;

	request->clientlen = sizeof(request->client);

	n = recvfrom(net_info->sock, &request->data,
		     sizeof(request->data), 0,
		     &request->client, &request->clientlen);

	/* sizeof of request must be size of a DNBD request */
	return (n == sizeof(request->data) ? 1 : 0);
}

/* 
 * function net_init(): initialize network for multicast 
 * returns: structure with network related information
 */
net_info_t *net_init(const char *mnet)
{
	struct ip_mreq mreq;
	const int ttl = 64;	/* TTL of 64 should be enough */
	u_char loop = 0;

	net_info_t *net_info = NULL;

	net_info = (net_info_t *) malloc(sizeof(net_info_t));
	if (!net_info)
		return NULL;

	memset(net_info, 0, sizeof(net_info_t));

	/* network setup */
	net_info->server.sin_family = AF_INET;
	net_info->server.sin_port = htons(DNBD_PORT);
	net_info->sock = socket(PF_INET, SOCK_DGRAM, 0);

	if (!inet_aton(mnet, &net_info->server.sin_addr)) {
		fprintf(stderr,
			"ERROR: multicast group %s is not a valid address!\n",
			mnet);
		goto out_free;
	}

	if (bind
	    (net_info->sock, (struct sockaddr *) &net_info->server,
	     sizeof(net_info->server)) < 0) {
		fprintf(stderr, "ERROR: binding socket!\n");
		goto out_free;
	}

	if (!inet_aton(mnet, &net_info->groupnet.sin_addr)) {
		fprintf(stderr,
			"ERROR: multicast group %s is not a valid address!\n",
			mnet);
		goto out_free;
	}

	/* multicast setup */
	net_info->groupnet.sin_family = AF_INET;
	net_info->groupnet.sin_port = htons(DNBD_PORT);

	mreq.imr_interface.s_addr = htonl(INADDR_ANY);
	memcpy(&mreq.imr_multiaddr, &net_info->groupnet.sin_addr,
	       sizeof(struct in_addr));

	if (setsockopt
	    (net_info->sock, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq,
	     sizeof(mreq)) < 0) {
		fprintf(stderr,
			"ERROR: cannot add multicast membership!\n");
		goto out_free;
	}

	if (setsockopt(net_info->sock, IPPROTO_IP, IP_MULTICAST_TTL,
		       &ttl, sizeof(ttl)) < 0) {
		fprintf(stderr, "ERROR: Setting TTL to 2\n");
		goto out_free;
	}

	/* no looping, please */
	if (setsockopt
	    (net_info->sock, IPPROTO_IP, IP_MULTICAST_LOOP, &loop,
	     sizeof(loop)) < 0) {
		fprintf(stderr,
			"ERROR: cannot disable multicast looping!\n");
		goto out_free;
	}

	goto out;

      out_free:
	fprintf(stderr,
		"hint: check kernel multicast support, multicast routing\n");
	if (net_info)
		free(net_info);

	net_info = NULL;
      out:
	return net_info;
}
