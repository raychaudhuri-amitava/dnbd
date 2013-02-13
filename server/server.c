/*
 * main.c - central part of the DNBD server application
 * Copyright (C) 2006 Thorsten Zitterell <thorsten@zitterell.de>
 */

#include <stdio.h>
#include <string.h>
#include <getopt.h>
#include <unistd.h>
#include <stdlib.h>

/* network includes */
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <signal.h>

#define DNBD_USERSPACE		1
#include "../common/dnbd-cliserv.h"

#include "server.h"
#include "query.h"
#include "net.h"
#include "filer.h"


static int verbose = 0;
static int running = 1;

/*
 * function: handle_signal(): set global variable running to 0 if signal arrives
 */
void handle_signal(int signum)
{
	running = 0;
}

void server_help(void)
{
	fprintf(stderr, "dnbd-server, version %s\n", DNBD_VERSION);
	fprintf(stderr,
		"Usage: dnbd-server -m <address> -d <device/file> -i <number>\n");
	fprintf(stderr, "\n");
	fprintf(stderr, "description:\n");
	fprintf(stderr, "  -m|--mcast     <multicast-address>\n");
	fprintf(stderr, "  -d|--device    <block device or file>\n");
	fprintf(stderr, "  -i|--id        <unique identification number>\n");
	fprintf(stderr, "  -t|--threads   <number of threads>\n");
}

/*
 * function: server_init(): parse command lines
 */
server_info_t *server_init(int argc, char **argv)
{
	/* cmd
	 * -1: error
	 * 0: not defined 
	 * 1: serve
	 */
	int cmd = 0;
	server_info_t *server_info = NULL;

	server_info = (server_info_t *) malloc(sizeof(server_info_t));
	if (!server_info)
		return NULL;

	memset(server_info, 0, sizeof(server_info_t));
	
	server_info->threads = 1;

	/* return value for getopt */
	int c;

	while (1) {
		static struct option long_options[] = {
			{"verbose", no_argument, 0, 'v'},
			{"mcast", required_argument, 0, 'm'},
			{"device", required_argument, 0, 'd'},
			{"threads", required_argument, 0, 't'},
			{"id", required_argument, 0, 'i'},
			{0, 0, 0, 0}
		};
		/* option index for getopt_long */
		int option_index = 0;

		c = getopt_long(argc, argv, "vm:d:i:t:",
				long_options, &option_index);

		/* at end of options? */
		if (c == -1)
			break;

		/* 
		   cmd = (cmd ? -1 : xx) is used to set cmd when it was
		   unset (0) before. Otherwise save error value 
		 */
		switch (c) {
		case 'v':
			verbose++;
			break;
		case 'm':
			server_info->mnet = optarg;	/* multicast address */
			break;
		case 'd':
			cmd = (cmd ? -1 : 2);	/* device/file */
			server_info->filename = optarg;
			break;
		case 'i':
			if (sscanf(optarg, "%u",&server_info->id) != 1) {
				fprintf(stderr,"ERROR: Id not a 16bit-integer (>0)\n");
				cmd = -1;
			}
			break;
		case 't':
			if (sscanf(optarg, "%u",&server_info->threads) != 1) {
				fprintf(stderr,"ERROR: Number of threads is wrong (>0)\n");
				cmd = -1;
			}
			break;

		default:
			cmd = -1;
		}
		
		if (cmd < 0) break;
	}

	/* no/wrong command given? */
	if (cmd <= 0) {
		server_help();
		goto out_free;
	}

	if (!server_info->mnet) {
		fprintf(stderr, "ERROR: multicast group was not set!\n");
		goto out_free;
	}

	if (!(server_info->id > 0)) {
		fprintf(stderr, "ERROR: unique id not set or not valid!\n");
		goto out_free;
	}
	
	if (!(server_info->threads > 0)) {
		fprintf(stderr, "ERROR: number of threads is not valid!\n");
		goto out_free;
	}


	/* call function for command */
	goto out;

      out_free:
	if (server_info)
		free(server_info);
	server_info = NULL;
      out:
	return server_info;
}

/*
 * function: main(): server startup
 */
int main(int argc, char **argv)
{

	server_info_t *server_info;
	
	signal(SIGINT, handle_signal);

	/* parse and verify command line options */
	if (!(server_info = server_init(argc, argv))) {
		fprintf(stderr, "ERROR: Parsing arguments!\n");
		goto out_server;
	}

	/* initialize network configuration and start listener thread */
	if (!(server_info->net_info = net_init(server_info->mnet))) {
		fprintf(stderr, "ERROR: Initializing net!\n");
		goto out_net;
	}

	if (!(server_info->filer_info = filer_init(server_info->filename))) {
		fprintf(stderr, "ERROR: Initializing filer!\n");
		goto out_filer;
	}

	/* initialize threads to handle requests */
	if (!
	    (server_info->query_info =
	     query_init(server_info->net_info, server_info->filer_info,
			server_info->id, server_info->threads))) {
		fprintf(stderr, "ERROR: Initializing query!\n");
		goto out_query;
	}

	while (running)
		pause();
	
	fprintf(stdout, "cleaning up...\n");
      out_query:
	if (server_info->filer_info)
		free(server_info->filer_info);

      out_filer:
	if (server_info->net_info)
		free(server_info->net_info);

      out_net:
	if (server_info) {
		free(server_info);
	}
      out_server:
	return 0;
}
