#ifndef LINUX_DNBD_NET_H
#define LINUX_DNBD_NET_H	1

#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>

/* network information */
struct net_info {
	int sock;
	struct sockaddr_in server;
	struct sockaddr_in groupnet;
};
typedef struct net_info net_info_t;
	
/* structure for received network packet */
struct net_request {
	struct sockaddr_in client;
	socklen_t clientlen;
	dnbd_request_t data;
	size_t len;
};
typedef struct net_request net_request_t;

/* structure for network packets to be sent */
struct net_reply {
	void *data;
	size_t len;
};
typedef struct net_reply net_reply_t;


/* struct net_info_s net_info; */

net_info_t * net_init();

/* functions */
void net_tx(net_info_t *net, net_reply_t *reply);
int net_rx(net_info_t * net, net_request_t *request);
	
/* network to host byte order */
#include <endian.h>
#if __BYTE_ORDER == __BIG_ENDIAN
#define ntohll(x) (x)
#else
#define ntohll(x) bswap_64(x)
#endif



#endif
