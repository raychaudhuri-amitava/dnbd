#ifndef LINUX_DNBD_REQUEST_H
#define LINUX_DNBD_REQUEST_H	1	

#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <time.h>

#include "net.h"
#include "filer.h"

struct query_info {
	pthread_t p_thread;
	net_info_t *net_info;
	filer_info_t *filer_info;
	int id;
};

typedef struct query_info query_info_t;

/* query information for requests and replies */
struct query {
	time_t time;
	net_request_t request;
	net_reply_t reply;
};

typedef struct query query_t;

/* functions */
query_info_t *query_init(net_info_t *, filer_info_t *, int id, int threads);

/* host to network byte order */
#include <endian.h>
#if __BYTE_ORDER == __BIG_ENDIAN
#define htonll(x) (x)
#else
#define htonll(x) bswap_64(x)
#endif


#endif
