#ifndef LINUX_DNBD_SERVER_H
#define LINUX_DNBD_SERVER_H	1

#include "filer.h"
#include "net.h"
#include "query.h"

/* server relevant information mainly given by command line */
struct server_info {
	const char *filename;
	int id;
	int threads;
	const char *mnet;
	filer_info_t *filer_info;
	net_info_t *net_info;
	query_info_t *query_info;
};	

typedef struct server_info server_info_t;
	
#endif
