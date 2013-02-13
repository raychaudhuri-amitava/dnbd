#ifndef LINUX_DNBD_NET_H
#define LINUX_DNBD_NET_H	1

#include <linux/spinlock.h>
#include <asm/semaphore.h>
#include <linux/list.h>
#include <linux/param.h>
#include <linux/jiffies.h>

#define 	SERVERS_MAX		8
#define		SERVER_STALLED		-1
#define		SERVER_INACTIVE		0
#define		SERVER_ACTIVE		1

/* limits and other constants for SRTT calculations */
#define		TIMEOUT_MIN		1
#define		TIMEOUT_MAX		HZ / 4
#define		TIMEOUT_STALLED		5 * HZ
#define		TIMEOUT_SHIFT		2

/* beta is 99% (990/1000) */
#define		SRTT_BETA		990
#define		SRTT_BETA_BASE		1000	
#define		SRTT_SHIFT		10

/* normalize weights to 255 as there is no float arithmetic in kernel */
#define		WEIGHT_NORMAL		((1<<8)-1)
#define		WEIGHT_FACTOR		(1<<20)

#define dnbd_rx_update(servers, id) \
if ((id > 0) && (id <= SERVERS_MAX)) servers.serverlist[id-1].last_rx = jiffies;
	
#define dnbd_tx_update(servers, id) \
if ((id > 0) && (id <= SERVERS_MAX)) servers.serverlist[id-1].last_tx = jiffies;

/* characteristics of a server */
struct dnbd_server {
	int id;
	int state;
	int srtt;
	int weight;
	unsigned long last_rx;		/* in jiffies */
	unsigned long last_tx;		/* in jiffies */
};

typedef struct dnbd_server dnbd_server_t;

/* common server information and helper variables */
struct dnbd_servers {
	struct dnbd_server *serverlist;
	struct dnbd_server *server;
	spinlock_t lock;
	struct semaphore sema;
	int timeout_min;
	int timeout_max;
	int timeout_stalled;
	int asrtt;
	int count;
};

typedef struct dnbd_servers dnbd_servers_t;
	
/* functions */
int dnbd_set_serverid(dnbd_servers_t * servers, int id);
int dnbd_next_server(dnbd_servers_t * servers);
void dnbd_rem_servers(dnbd_servers_t * servers);
void dnbd_rtt_server(dnbd_servers_t * servers, int id, int rtt);
int dnbd_servers_init(dnbd_servers_t *servers);
void dnbd_servers_weight(dnbd_servers_t * servers);
int dnbd_show_servers(dnbd_servers_t * servers, void *buf, int size);
void dnbd_clean_servers(dnbd_servers_t * servers);
	
#endif
