#ifndef LINUX_DNBD_QUEUE_H
#define LINUX_DNBD_QUEUE_H	1

#include <linux/spinlock.h>
#include <asm/semaphore.h>
#include <linux/list.h>
#include <linux/wait.h>

/* queue structure used for rx_queue and tx_queue */
struct dnbd_queue {
	spinlock_t lock;
	struct semaphore sema;
	struct list_head head;
	wait_queue_head_t waiters;
};

typedef struct dnbd_queue dnbd_queue_t;

/* functions */
void dnbd_enq_request(dnbd_queue_t * q, struct request *req, int wakeup);
struct request *dnbd_deq_request(dnbd_queue_t * q);
struct request *dnbd_deq_request_handle(dnbd_queue_t * q, uint64_t pos);
struct request *dnbd_try_deq_request(dnbd_queue_t * q);
void dnbd_mark_old_requests(dnbd_queue_t * q);
int dnbd_requeue_requests(dnbd_queue_t * to, dnbd_queue_t * from, unsigned long timeout);
void dnbd_error_old_requests(dnbd_queue_t * q);


#endif				/* LINUX_QUEUE_H */
