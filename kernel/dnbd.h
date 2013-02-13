#ifndef LINUX_DNBD_H
#define LINUX_DNBD_H	1

#include <linux/completion.h>
#include <linux/in.h>
#include <asm/semaphore.h>
#include <linux/blkdev.h>
#include <linux/rbtree.h>
#include <linux/jiffies.h>
#include <linux/timer.h>
#include <linux/version.h>

#include "queue.h"
#include "cache.h"
#include "net.h"

#define MAX_DNBD 16
#define PRINTK(level,fmt,args...) \
	        printk(level "dnbd%d: " fmt, \
		                (DEVICE_TO_MINOR(dnbd)) , ##args)
#define INFO(fmt,args...) PRINTK(KERN_INFO, fmt , ##args)

/* needed for rx_loop, tx_loop, ss_loop */
struct dnbd_thread {
	struct task_struct *task;
	struct completion startup;
	struct completion finish;
};

typedef struct dnbd_thread dnbd_thread_t;

struct dnbd_device {
	int magic;
	int state;
	struct socket *sock;		/* network socket */
	struct sockaddr_in mcast;
	struct file *file;		
	spinlock_t thread_lock;		/* locks */
	spinlock_t queue_lock;
	spinlock_t timer_lock;	
	struct semaphore semalock;
	struct gendisk *disk;		/* general disk interface */
	int blksize;
	u64 bytesize;
	atomic_t refcnt;		/* reference counter for module */
	dnbd_thread_t rx_thread;	
	dnbd_thread_t tx_thread;
	dnbd_thread_t ss_thread;
	atomic_t num_io_threads;
	wait_queue_head_t io_waiters;
	dnbd_queue_t rx_queue;		/* queue for outstanding request */
	dnbd_queue_t tx_queue;		/* queue for requests to be sent */
	struct dnbd_cache cache;
	struct dnbd_servers servers;	/* pointer to servers */
	struct timer_list timer;
};

typedef struct dnbd_device dnbd_device_t;
	

#endif				/* LINUX_DNBD_H */
