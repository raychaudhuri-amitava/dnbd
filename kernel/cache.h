#ifndef LINUX_DNBD_CACHE_H
#define LINUX_DNBD_CACHE_H	1

#include <linux/rbtree.h>
#include <linux/blkdev.h>

#include "../common/dnbd-cliserv.h"

/* node for red-black tree */
struct cache_node {
	struct rb_node rb_node;
	sector_t rb_key;
	sector_t rb_data;
	/* previous and next node used for LRU */
	struct cache_node *prev;
	struct cache_node *next;
};

typedef struct cache_node cache_node_t;

/* cache characteristics */
struct dnbd_cache {
	int active;				/* !0 when cache active */
	char *fname;				/* cache file name */
	int fd;					/* cache file descriptor */
	struct file *filp;			/* cache file structure */
	struct rb_root root;			/* root node of cache */
	sector_t max_blocks;			/* maximum of cached blocks */
	sector_t used_blocks;			/* current used blocks */
	size_t blksize;
	struct cache_node *head;		/* head of LRU list */
	struct cache_node *tail;		/* tail of LRU list */
	spinlock_t lock;
	struct semaphore sema;
	int (*insert) (struct dnbd_cache * cache, sector_t sector, void *buf);
	int (*search) (struct dnbd_cache * cache, struct request *req);	
	void (*clean) (struct dnbd_cache * cache);
	long hits;				/* statistics */
	long miss;				
	long lru;
	
};

typedef struct dnbd_cache dnbd_cache_t;

int dnbd_cache_init(dnbd_cache_t * cache);
int dnbd_cache_set(dnbd_cache_t * dcache, struct dnbd_file __user * cachefile, size_t blksize);

#endif
