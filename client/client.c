/*
 * client.c - controlling application for block device driver
 * Copyright (C) 2006 Thorsten Zitterell <thorsten@zitterell.de>
 */

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <getopt.h>
#include <errno.h>
#include <time.h>

/* network includes */
#include <netdb.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/poll.h>


/* file operations */
#include <sys/time.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>

#include <unistd.h>
#include <sys/ioctl.h>

#include <linux/kdev_t.h>

#define DNBD_USERSPACE		1
#include "../common/dnbd-cliserv.h"
#include "client.h"

/* device driver setup information */
struct client_s {
	const char *mnetname;		/* multicast address */
	const char *cachefile;		/* name of cache file */
	struct sockaddr_in mca_adr;	/* multicast address */
	int mca_len;			/* and its byte length */
	const char *devicename;		/* name of the device */
	int dnbd;			/* file descriptor of dnbd device */
	int port;			/* used port for multicast */
	int sock;			/* socket descriptor */
	uint64_t capacity;		/* capacity of device */
	uint16_t blksize;		/* blocksize of device */
};

typedef struct client_s client_t;
	
/* beeing more verbose, if necessary */
static int verbose = 0;

/* structure of request to server */
struct dnbd_request request;

/* 
 * function daemonize(): forks our process that it can run in background
 * returns: 1 on success, otherwise 0;
 */
int daemonize(void)
{
	pid_t pid;

	pid = fork();

	if (pid > 0) {
		exit(0);
	}
	if (pid < 0) {
		fprintf(stderr, "fork() failed!\n");
		return 0;
	}
	return 1;
}

/* 
 * function open_dnbd(): open the block device and do some prechecking
 * returns: file descriptor of block device 
 */
int open_dnbd(client_t * client)
{
	int dnbd;
	struct stat statbuf;

	/* try to open the network block device */
	if ((dnbd = open(client->devicename, O_RDONLY)) < 0) {
		if (errno == ENXIO)
			fprintf(stderr,
				"ERROR: make sure dnbd module is loaded!\n");
		fprintf(stderr,
			"ERROR: Dnbd devide could not be opened!\n");
		return -EINVAL;
	}

	/* error, if we cannot get status of file */
	if (fstat(dnbd, &statbuf) == -1) {
		fprintf(stderr, "ERROR: Can not stat dnbd!\n");
		return -EINVAL;
	}

	/* error, if it is not a block device */
	if (!(S_ISBLK(statbuf.st_mode)))
		return -EINVAL;

	return dnbd;
}

/* 
 * function open_net(): configures network parameters
 * returns: socket descriptor of multicast net (int)
 */
int open_net(client_t * client)
{
	int sock;
	const int ttl = 64;	/* a TTL of 64 for multicast should be enough */
	struct ip_mreq mreq;
	u_char loop = 0;	/* multicast looping is disabled */

	/* zero multicast address and convert to appropriate type */
	memset(&client->mca_adr, 0, sizeof(client->mca_adr));
	if (inet_aton(client->mnetname, &client->mca_adr.sin_addr) < 0) {
		fprintf(stderr, "ERROR: Wrong multicast address \"%s\"!",
			client->mnetname);
		return -EINVAL;
	}

	/* configure multicast socket */
	client->mca_adr.sin_family = AF_INET;
	client->mca_adr.sin_port = htons(client->port);
	if ((sock = socket(AF_INET, SOCK_DGRAM, 0)) < 0) {
		fprintf(stderr, "ERROR: Socket creation failed!\n");
		return -EINVAL;
	}

	/* bind socket */
	if (bind
	    (sock, (struct sockaddr *) &client->mca_adr,
	     sizeof(client->mca_adr)) < 0) {
		fprintf(stderr, "ERROR: Socket bind failed!\n");
		return -EINVAL;
	}

	/* setup multicast, join multicast group, set TTL and disable looping */
	if (inet_aton(client->mnetname, &mreq.imr_multiaddr) < 0) {
		fprintf(stderr, "ERROR: Wrong multicast address \"%s\"!",
			client->mnetname);
		return -EINVAL;
	}
	mreq.imr_interface.s_addr = htonl(INADDR_ANY);
	if (setsockopt
	    (sock, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq,
	     sizeof(mreq)) < 0) {
		fprintf(stderr, "ERROR: Adding multicast membership\n");
		return -1;
	}
	if (setsockopt(sock, IPPROTO_IP, IP_MULTICAST_TTL,
		       &ttl, sizeof(ttl)) < 0) {
		fprintf(stderr, "ERROR: Setting TTL to %i\n",ttl);
		return -1;
	}	
	setsockopt(sock, IPPROTO_IP, IP_MULTICAST_LOOP, &loop,
		   sizeof(loop));


	return sock;
}

/* 
 * function do_handshake(): send init requests to the network and wait for 
 *          server replies
 * returns: 0 on success, otherwise POSIX error code
 */
int do_handshake(client_t * client)
{
	int result;
	struct pollfd read_fds[1];
	struct dnbd_request request;
	int replylen;
	struct dnbd_reply_init reply_init;
	time_t starttime, stoptime;
	int cycle = 1;
	int servers = 0;

	client->capacity = 0;
	client->blksize = 0;

	/* for network socket polls */
	read_fds[0].fd = client->sock;
	read_fds[0].events = POLLIN;
	read_fds[0].revents = 0;

	/* request comes from a client and is addressed to all servers */
	request.magic = htonl(DNBD_MAGIC);
	request.cmd = htons(DNBD_CMD_INIT | DNBD_CMD_CLI);
	request.id = htons(0);	/* ask all servers */

	/* send requests (in 1 second intervals) */ 
	/* and wait DNBD_TIMEOUT seconds for replies */
	printf("Searching for servers...\n");
	starttime = time(NULL);
	(void) sendto(client->sock, &request, sizeof(request),
		      0, (struct sockaddr *) &client->mca_adr,
		      sizeof(client->mca_adr));


	while (cycle) {

		stoptime = time(NULL);

		/* timeout after DNBD_TIMEOUT seconds */
		if ((stoptime - starttime) > DNBD_TIMEOUT)
			break;

		/* wait for reply or send a request each second */
		if ((result = poll(read_fds, 1, 1000)) != 1) {
			(void) sendto(client->sock, &request,
				      sizeof(request), 0,
				      (struct sockaddr *) &client->mca_adr,
				      sizeof(client->mca_adr));
			continue;
		}

		/* handle reply */
		replylen =
		    recv(client->sock, &reply_init,
			 sizeof(struct dnbd_reply_init), MSG_WAITALL);
			
		/* check for integrity */
		if (replylen != sizeof(struct dnbd_reply_init))
			continue;
		reply_init.magic = ntohl(reply_init.magic);
		if (reply_init.magic != DNBD_MAGIC) {
			continue;
		}
		reply_init.cmd = ntohs(reply_init.cmd);
		if (!((reply_init.cmd & DNBD_CMD_SRV) &&
		      ((reply_init.cmd & DNBD_CMD_MASK) == DNBD_CMD_INIT)))
			continue;

		/* copy parameters of block device from reply */
		reply_init.id = ntohs(reply_init.id);
		reply_init.capacity = ntohll(reply_init.capacity);
		reply_init.blksize = ntohs(reply_init.blksize);

		/* add server to block device servers */
		if ((result =
		     ioctl(client->dnbd, DNBD_SET_SERVERID,
			   reply_init.id)) < 0) {
			if (errno == EEXIST)
				continue;
			else
				fprintf(stderr,
					"ERROR: ioctl DNBD_SET_SERVERID failed!\n");
			return -EINVAL;
		} else {
			printf("* Added server with id %i\n",
			       reply_init.id);

			client->capacity = reply_init.capacity;
			client->blksize = reply_init.blksize;
			servers++;
		}

	}

	/* check, if servers have been found */
	if (servers) {
		printf("Capacity of device is %llu, blksize is %i\n",
		       client->capacity, client->blksize);
		result = 0;
	} else {
		if (ioctl(client->dnbd, DNBD_DISCONNECT) < 0) {
			fprintf(stderr,
				"ERROR: ioctl DNBD_DISCONNECT failed!\n");
		}
		fprintf(stderr, "No servers found!\n");
		result = -ECONNABORTED;
	}

	return result;
}

/* 
 * function do_bind(): open block devicename, bind network, do handshake,
 *          set block device configuration (size, blocksize),
 *          start session, fork and go to background
 * returns: 1 when session finished, otherwise POSIX error code
 */
int do_bind(client_t * client)
{
	struct dnbd_file cfile;
		
	/* open block device */
	if ((client->dnbd = open_dnbd(client)) < 0)
		return -EINVAL;
	printf("DNBD device successfully set.\n");

	/* bind network */
	if ((client->sock = open_net(client)) < 0)
		return -EINVAL;
	fprintf(stdout, "Socket successfully opened.\n");

	/* configure block device */
	if (ioctl(client->dnbd, DNBD_SET_SOCK, client->sock) < 0) {
		close(client->sock);
		fprintf(stderr, "ERROR: ioctl DNBD_SET_SOCKET failed!\n");
		return -EINVAL;
	}
	if (ioctl(client->dnbd, DNBD_SET_GROUPNET, &client->mca_adr) < 0) {
		fprintf(stderr,
			"ERROR: ioctl DNBD_SET_GROUPNET failed!\n");
		return -EINVAL;
	}
	fprintf(stdout, "Multicast address successfully set to %s.\n",
		inet_ntoa(client->mca_adr.sin_addr));

	/* start handshake */
	if (do_handshake(client) < 0)
		return -EINVAL;

	/* set block size and capacity of device */
	if (ioctl(client->dnbd, DNBD_SET_BLKSIZE, client->blksize) < 0) {
		fprintf(stderr, "ERROR: ioctl DNBD_SET_BLKSIZE failed!\n");
		return -EINVAL;
	}
	if (ioctl(client->dnbd, DNBD_SET_CAPACITY, &client->capacity) < 0) {
		fprintf(stderr, "ERROR: ioctl DNBD_SET_SIZE failed!\n");
		return -EINVAL;
	}

	/* activate cache, if necessary */
	if (client->cachefile) {
		cfile.name = client->cachefile;
		cfile.len = strlen(client->cachefile);
		if (ioctl(client->dnbd, DNBD_SET_CACHE, &cfile) < 0) {
			fprintf(stderr,
				"ERROR: ioctl DNBD_SET_CACHE failed!\n");
			return -EINVAL;
		}
		printf("Cachefile successfully set.\n");
	}

	/* go to background */
	if (!daemonize())
		return -ECHILD;
	if (ioctl(client->dnbd, DNBD_DO_IT) < 0) {
		fprintf(stderr,
			"ERROR: ioctl DNBD_DO_IT terminated unexpected!\n");
	} else {
		fprintf(stdout, "dnbd terminated.\n");
	}

	return 1;
}

/* 
 * function do_unbind(): end session
 * returns: 1 on success, otherwise POSIX error code
 */
int do_unbind(client_t * client)
{
	/* open block device */
	if ((client->dnbd = open_dnbd(client)) < 0)
		return -EINVAL;
	fprintf(stdout, "dnbd device successfully opened.\n");
	
	/* send disconnect */
	if (ioctl(client->dnbd, DNBD_DISCONNECT) < 0) {
		fprintf(stderr, "ERROR: ioctl DNBD_DISCONNECT failed!\n");
		return -EINVAL;
	}

	return 1;
}

/* 
 * function: do_setcache(): set cache when block device is already active
 * returns: 1 on success, otherwise POSIX error code
 */
int do_setcache(client_t * client)
{
	struct dnbd_file cfile;
	if ((client->dnbd = open_dnbd(client)) < 0)
		return -EINVAL;
	fprintf(stdout, "dnbd device successfully opened.\n");

	if (client->cachefile) {
		cfile.name = client->cachefile;
		cfile.len = strlen(client->cachefile);
		if (ioctl(client->dnbd, DNBD_SET_CACHE, &cfile) < 0) {
			fprintf(stderr,
				"ERROR: ioctl DNBD_SET_CACHE failed!\n");
			return -EINVAL;
		}
		printf("Cachefile successfully set.\n");
	}

	return 1;
}

/* 
 * function: client_help
 */
void client_help(void)
{
	fprintf(stderr, "dnbd-client, version %s\n", DNBD_VERSION);
	fprintf(stderr,
		"Usage: dnbd-client -d device -b <address> [-c <file>]\n");
	fprintf(stderr, "    or dnbd-client -d device -u\n");
	fprintf(stderr, "    or dnbd-client -d device -c <file>\n");
	fprintf(stderr, "\n");
	fprintf(stderr, "description:\n");
	fprintf(stderr, "  -d|--device    <device>\n");
	fprintf(stderr, "  -b|--bind      <multicast-address>\n");
	fprintf(stderr, "  -u|--unbind    \n");
	fprintf(stderr, "  -c|--cache     <file>\n");
	fprintf(stderr, "\n");
}

/* 
 * function client_shutdown()
 */
void client_shutdown(client_t * client)
{
	if (client->dnbd > 0)
		close(client->dnbd);
}

/* 
 * function parseopts(): parse command line options
 * returns: command identifier or error <= 0
 *   -1: error
 *    0: not defined
 *    1: bind a block device
 *    2: unbind a block devicename
 *    3: set cache file
 */
int parseopts(int argc, char **argv, client_t * client)
{
	int cmd = 0, err = 0;
	
	memset(client, 0, sizeof(client_t));
	client->port = DNBD_PORT;
	/* return value for getopt */
	int c;
	/* start option parsing */
	while (1) {
		static struct option long_options[] = {
			{"verbose", no_argument, 0, 'v'},
			{"bind", required_argument, 0, 'b'},
			{"unbind", no_argument, 0, 'u'},
			{"cache", required_argument, 0, 'c'},
			{"device", required_argument, 0, 'd'},
			{0, 0, 0, 0}
		};
		/* option index for getopt_long */
		int option_index = 0;
		opterr = 0;
		c = getopt_long(argc, argv, "b:ud:c:v",
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
		case 'b':
			cmd = (cmd ? -1 : 1);	/* bind */
			client->mnetname = optarg;
			break;
		case 'u':
			cmd = (cmd ? -1 : 2);	/* unbind */
			break;
		case 'd':
			cmd = (client->devicename ? -1 : cmd);
			client->devicename = optarg;
			break;
		case 'c':
			cmd = (client->cachefile ? -1 : cmd);
			client->cachefile = optarg;
			break;
		case '?':
			fprintf(stderr, "ERROR: wrong parameters\n");
		default:
			cmd = -1;
		}

	}

	/* no/wrong command given? */
	if (cmd <= 0) {
		/* set cache file, when not (un)bind */
		if (client->cachefile) 
			cmd = 3;
		else
			err = -EINVAL;
	}

	if (cmd > 0 && !client->devicename) {
		fprintf(stderr, "ERROR: no device given!\n");
		err = -EINVAL;
	}

	if (err < 0) {
		fprintf(stderr, "\n");
		client_help();
		return -err;
	}


	return (cmd);
}

/* 
 * function main(): calls option parser, 
 *          executes subrotine (bind/unbind/set cache)
 * returns: 0 on success, otherwise POSIX error code
 */

int main(int argc, char **argv)
{
	client_t client;
	int cmd, err = 0;
	cmd = parseopts(argc, argv, &client);
	if (cmd < 0)
		return 1;
	/* call function for command */
	switch (cmd) {
	case 1:
		err = do_bind(&client);
		break;
	case 2:
		err = do_unbind(&client);
		break;
	case 3:
		err = do_setcache(&client);
		break;
	}

	client_shutdown(&client);
	return err;
}
