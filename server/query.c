/*
 * query.c - request/reply handling for the server
 * Copyright (C) 2006 Thorsten Zitterell <thorsten@zitterell.de>
 */
 
#include <stdio.h>		
#include <pthread.h>		
#include <stdlib.h>	
#include <string.h>

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include <linux/types.h>
#include <unistd.h>
#include <time.h>

#define DNBD_USERSPACE		1
#include "../common/dnbd-cliserv.h"

#include "query.h"

/* number of threads used to service requests */
#define NUM_HANDLER_THREADS	1		/* default */
#define MAX_BLOCK_SIZE		4096

struct query_thread {
	query_info_t *query_info;
	int id;
	pthread_t p_thread;
};

struct query_thread query_thread[NUM_HANDLER_THREADS];

/* recursive global mutex for our program. */
pthread_mutex_t query_mutex = PTHREAD_RECURSIVE_MUTEX_INITIALIZER_NP;
/* mutex to avoid concurrent file access */
pthread_mutex_t handler_mutex = PTHREAD_MUTEX_INITIALIZER;

/* global condition variable for our program. */
pthread_cond_t got_query = PTHREAD_COND_INITIALIZER;

int num_queries = 0;		/* number of pending requests, initially none */
int max_queries = 100;		/* this value should be high enough */

query_t *queries = NULL;	/* head of linked list of requests. */
int last_query = 0;		/* initial position in circular buffer */
int next_query = 0;		


void query_handle(struct query_info *query_info, query_t * query);

/* 
 * function query_add_loop(): add incoming requests to circular buffer 
 */
void *query_add_loop(void *data)
{
	int rc;
	query_t *query;
	query_info_t *query_info = (query_info_t *) data;

	int tmp_query;

	while (1) {

		rc = pthread_mutex_lock(&query_mutex);
		tmp_query = (next_query + 1) % max_queries;
		rc = pthread_mutex_unlock(&query_mutex);

		if (tmp_query == last_query)
			continue;

		query = &queries[next_query];

		/* loop until a proper request arrives */
		while (!net_rx(query_info->net_info, &query->request)) {}

		rc = pthread_mutex_lock(&query_mutex);

		next_query = tmp_query;

		/* increase total number of pending requests */
		num_queries++;

		rc = pthread_mutex_unlock(&query_mutex);

		/* signal that there's a new request to handle */
		rc = pthread_cond_signal(&got_query);
	}
}

/*
 * function: query_get(): fetch request from circular buffer
 * returns: pointer to request
 */
query_t *query_get(pthread_mutex_t * p_mutex)
{
	int rc;		
	query_t *query;		/* pointer to request */

	rc = pthread_mutex_lock(p_mutex);

	if (last_query == next_query)
		return NULL;

	query = &queries[last_query];

	last_query = (last_query + 1) % max_queries;
	num_queries--;

	rc = pthread_mutex_unlock(p_mutex);
	/* return the request to the caller */
	return query;
}

/*
 * function query_handle(): handle a single request.
 */
void query_handle(struct query_info *query_info, query_t * query)
{
	int i, rc;
	dnbd_request_t *dnbd_request;
	dnbd_request_t *dnbd_old_request;
	dnbd_reply_t *dnbd_reply = NULL;
	struct dnbd_reply_init *dnbd_reply_init;
	int tmp_query;
	int recent = 0;
	time_t timestamp;

	dnbd_request = (dnbd_request_t *) & query->request.data;

	query->reply.len = 0;

	/* convert data from network to host byte order */
	dnbd_request->magic = ntohl(dnbd_request->magic);
	dnbd_request->time = ntohs(dnbd_request->time);
	dnbd_request->id = ntohs(dnbd_request->id);
	dnbd_request->cmd = ntohs(dnbd_request->cmd);
	dnbd_request->pos = ntohll(dnbd_request->pos);
	dnbd_request->len = ntohs(dnbd_request->len);

	if (dnbd_request->magic != DNBD_MAGIC)
		return;

	/* we ususally only respond to a client */
	if (!(dnbd_request->cmd & DNBD_CMD_CLI))
		return;

	/* does the client ask for our id? */
	if (dnbd_request->id && (dnbd_request->id != query_info->id))
		return;

	switch (dnbd_request->cmd & DNBD_CMD_MASK) {
	/* handle init request */
	case DNBD_CMD_INIT:
	/* handle heartbeat request */
	case DNBD_CMD_HB:
		dnbd_reply_init =
		    (struct dnbd_reply_init *) query->reply.data;
		dnbd_reply_init->magic = htonl(DNBD_MAGIC);

		dnbd_reply_init->capacity =
		    htonll(filer_getcapacity(query_info->filer_info));

		dnbd_reply_init->cmd =
		    htons((dnbd_request->cmd
			   & ~DNBD_CMD_CLI) | DNBD_CMD_SRV);

		dnbd_reply_init->blksize = htons(MAX_BLOCK_SIZE);
		dnbd_reply_init->id = htons(query_info->id);

		query->reply.len = sizeof(struct dnbd_reply_init);

		net_tx(query_info->net_info, &query->reply);
		break;
	/* handle read request */
	case DNBD_CMD_READ:
		timestamp = time(NULL);
	
		/* burst avoidance */
		rc = pthread_mutex_lock(&query_mutex);
		for (i = 2; i < max_queries; i++) {

			tmp_query =
			    (last_query + (max_queries - i)) % max_queries;
				
			if (tmp_query == last_query)
				break;

			/* check only up to one second */
			if (!tmp_query
			    || queries[tmp_query].time - timestamp > 1) {
				break;
			}
			dnbd_old_request =
			    (dnbd_request_t *) & queries[tmp_query].
			    request.data;

			/* someone requested the same block before? */
			if (dnbd_request->pos == dnbd_old_request->pos) {
				/* was it the same client, then retransmit
				as the packet was probably lost, otherwise
				drop the request */
				if (!((query->request.clientlen ==
				     queries[tmp_query].request.clientlen)
				    &&
				    (!memcmp
				     (&query->request.client,
				      &queries[tmp_query].request.client,
				      query->request.clientlen)))) {
					recent = 1;
					break;
				}
				else
					break;
			}
		} 
		rc = pthread_mutex_unlock(&query_mutex);

		if (recent)
			break;

		/* size of request block too high? */
		if (dnbd_request->len > MAX_BLOCK_SIZE)
			break;

		/* create a DNBD reply packet */
		dnbd_reply = (dnbd_reply_t *) query->reply.data;

		dnbd_reply->magic = htonl(DNBD_MAGIC);
		dnbd_reply->time = htons(dnbd_request->time);
		dnbd_reply->id = htons(query_info->id);
		dnbd_reply->pos = htonll(dnbd_request->pos);

		dnbd_reply->cmd =
		    htons((dnbd_request->cmd
			   & ~DNBD_CMD_CLI) | DNBD_CMD_SRV);

		/* read from underlying device/file */
		pthread_mutex_lock(&handler_mutex);
		filer_readblock(query_info->filer_info,
				(void *) dnbd_reply +
				sizeof(struct dnbd_reply),
				dnbd_request->len, dnbd_request->pos);

		pthread_mutex_unlock(&handler_mutex);

		query->reply.len =
		    dnbd_request->len + sizeof(dnbd_reply_t);

		query->time = time(NULL);

		/* send reply */
		net_tx(query_info->net_info, &query->reply);
		break;
	}


}

/*
 * function query_handle_loop(): get queries and handle them in a loop
 */
void *query_handle_loop(void *data)
{
	int rc;			
	query_t *query;				/* pointer to a request */
	int thread_id = *((int *) data);	/* thread id */

	printf("Starting thread '%d'\n", thread_id);
	fflush(stdout);

	rc = pthread_mutex_lock(&query_mutex);

	/* do forever.... */
	while (1) {

		if (num_queries > 0) {	
			/* a request is pending */
			query = query_get(&query_mutex);

			/* got a request? */
			if (query) {	
				
				rc = pthread_mutex_unlock(&query_mutex);
				/* handle request */
				query_handle(query_thread[thread_id].
					     query_info, query);

				rc = pthread_mutex_lock(&query_mutex);
			}
		} else {
			/* wait for a request to arrive */
			rc = pthread_cond_wait(&got_query, &query_mutex);
		}
	}
}

/*
 * function query_init(): initialize request handling
 * returns: pointer to data structure query_info (see header file)
 */
query_info_t *query_init(net_info_t * net_info, filer_info_t * filer_info,
			 int id, int threads)
{
	int i;
	query_info_t *query_info = NULL;

	query_info = (query_info_t *) malloc(sizeof(query_info_t));
	if (!query_info)
		return NULL;

	/* fill query_info structure */
	query_info->net_info = net_info;
	query_info->filer_info = filer_info;
	query_info->id = id;

	if (!(queries = (query_t *) malloc(sizeof(query_t) * max_queries))) {
		free(query_info);
		return NULL;
	}

	last_query = 0;
	next_query = 0;

	/* reserve memory for circular buffer */
	for (i = 0; i < max_queries; i++) {
		queries[i].reply.data =
		    malloc(MAX_BLOCK_SIZE + sizeof(dnbd_reply_t));
	}

	/* create the request-handling threads */
	for (i = 0; i < threads; i++) {

		query_thread[i].id = i;
		query_thread[i].query_info = query_info;

		pthread_create(&query_thread[i].p_thread, NULL,
			       query_handle_loop,
			       (void *) &query_thread[i].id);
	}

	/* create thread for receiving network requests */
	pthread_create(&query_info->p_thread, NULL,
		       query_add_loop, (void *) query_info);

	return query_info;
}
