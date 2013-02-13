/*
 * queue.c - queues for requests to be submitted (tx_queue) 
 *           and outstanding requests (rx_queue)
 * Copyright (C) 2006 Thorsten Zitterell <thorsten@zitterell.de>
 */

#include <linux/blkdev.h>
#include <linux/init.h>
#include <linux/sched.h>
#include <linux/fs.h>

#include <linux/spinlock.h>

#include <linux/in.h>

#include "dnbd.h"
#include "queue.h"

/* enqueue to a queue */
void dnbd_enq_request(dnbd_queue_t * q, struct request *req, int wakeup)
{
	unsigned long flags;
	spin_lock_irqsave(&q->lock, flags);
	list_add(&req->queuelist, &q->head);
	spin_unlock_irqrestore(&q->lock,flags);
	if (wakeup)
		wake_up(&q->waiters);
}

/* dequeue from a queue with position */
struct request *dnbd_deq_request_handle(dnbd_queue_t * q, uint64_t pos)
{
	struct request *req = NULL;
	struct list_head *tmp;
		unsigned long flags;

	spin_lock_irqsave(&q->lock,flags);
	list_for_each(tmp, &q->head) {
		req = blkdev_entry_to_request(tmp);
		if (((u64) req->sector) << 9 == pos) {
			list_del_init(&req->queuelist);
			goto out;
		}

	}
	req = NULL;
      out:
	spin_unlock_irqrestore(&q->lock,flags);
	return req;
}

/* dequeue from queue */
struct request *dnbd_deq_request(dnbd_queue_t * q)
{
	struct request *req = NULL;
	unsigned long flags;

	spin_lock_irqsave(&q->lock, flags);
	if (!list_empty(&q->head)) {
		req = blkdev_entry_to_request(q->head.prev);
		list_del_init(&req->queuelist);
	}
	spin_unlock_irqrestore(&q->lock, flags);
	return req;
}

/* sleep until request can be dequeued */
struct request *dnbd_try_deq_request(dnbd_queue_t * q)
{
	struct request *req;


	req = dnbd_deq_request(q);
	if (!req) {
		struct task_struct *tsk = current;

		DECLARE_WAITQUEUE(wait, tsk);
		add_wait_queue(&q->waiters, &wait);

		for (;;) {

			set_current_state(TASK_INTERRUPTIBLE);
			req = dnbd_deq_request(q);

			if (req || signal_pending(current))
				break;

			schedule();
		}

		set_current_state(TASK_RUNNING);
		remove_wait_queue(&q->waiters, &wait);
	}

	return req;
}

/* requeue requests with timeout */
int dnbd_requeue_requests(dnbd_queue_t * to, dnbd_queue_t * from,
			  unsigned long timeout)
{
	struct request *req = NULL;
	struct list_head *tmp, *keep;
	int requeued = 0;
	unsigned long flags;

	spin_lock_irqsave(&from->lock,flags);

	list_for_each_safe(tmp, keep, &from->head) {
		req = blkdev_entry_to_request(tmp);
		if (req->start_time < timeout) {
			requeued++;
			list_del_init(&req->queuelist);

			spin_lock_irqsave(&to->lock,flags);
			list_add(&req->queuelist, &to->head);
			spin_unlock_irqrestore(&to->lock,flags);
		}
	}

	spin_unlock_irqrestore(&from->lock,flags);

	wake_up(&to->waiters);

	return requeued;
}
