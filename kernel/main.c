 /*
  * main.c - central part of the dnbd device
  * Copyright (C) 2006 Milind Dumbare <milind@linsyssoft.com>
  *
  * Copyright (C) 2006 Thorsten Zitterell <thorsten@zitterell.de>
  *
  * see CREDITS for licence
  *
  * parts and ideas based on
  *
  * - ANBD (another network block device)
  * Copyright (C) 2003 Louis D. Langholtz <ld@aros.net>.
  *
  * - NBD (network block device)
  * Copytight 1979-2000 Pavel Machek <pavel@ucw.cz>
  * Parts copyright 2001 Steven Whitehouse <steve@chygwyn.com>
  *
  */

#include <linux/module.h>
#include <linux/proc_fs.h>
#include <linux/blkdev.h>
#include <linux/init.h>
#include <linux/sched.h>
#include <linux/preempt.h>
#include <linux/fs.h>		/* everything... */
#include <linux/bio.h>

#include <linux/errno.h>	/* error codes */
#include <linux/devfs_fs_kernel.h>
#include <asm/uaccess.h>
#include <linux/file.h>

/* network stuff */
#include <linux/net.h>
#include <linux/in.h>
#include <net/sock.h>
#include <linux/skbuff.h>
#include <linux/udp.h>

#include <linux/types.h>	/* size_t */

#include "../common/dnbd-cliserv.h"
#include "dnbd.h"
#include "queue.h"
#include "cache.h"
#include "net.h"

#define LO_MAGIC 0x68797548
#define DEVICE_TO_MINOR(dnbd) ((int)((dnbd)-dnbd_dev[0]))

int dnbd_major = DNBD_MAJOR;

/* private structures */
typedef int (*thread_fn_t) (void *);

/* function */
static int dnbd_rx_loop(void *data);
static int dnbd_tx_loop(void *data);

static struct dnbd_device *dnbd_dev[MAX_DNBD];
static struct proc_dir_entry *dnbd_proc_dir = NULL;

/* inform kernel that some sectors of a request have been transferred */
static int dnbd_end_request(dnbd_device_t * dnbd, struct request *req,
			    int success, int size)
{
	unsigned long flags;
	request_queue_t *q = req->q;

	int result = 0;

	spin_lock_irqsave(q->queue_lock, flags);
	if (!(result = end_that_request_first(req, success, size))) {
#if LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,16)
		end_that_request_last(req,success);
#else
		end_that_request_last(req);
#endif

	}
	spin_unlock_irqrestore(q->queue_lock, flags);
	return result;		/* 0, if request is completed */
}

/* empty a request queue */
void dnbd_clear_queue(dnbd_device_t * dnbd, dnbd_queue_t * q)
{
	struct request *req;
	do {
		req = dnbd_deq_request(q);
		if (req) {
			dnbd_end_request(dnbd, req, 0, req->nr_sectors);
		}
	} while (req);
}

/* empty all queues: tx_queue, rx_queue */
void dnbd_clear_queues(dnbd_device_t * dnbd)
{
	spin_lock_irq(&dnbd->thread_lock);

	if (dnbd->rx_thread.task) {
		printk(KERN_ERR
		       "dnbd_clear_queues: rx_thread still active!\n");
	} else {
		dnbd_clear_queue(dnbd, &dnbd->rx_queue);
	}

	if (dnbd->tx_thread.task) {
		printk(KERN_ERR
		       "dnbd_clear_queues: tx_thread still active!\n");
	} else {
		dnbd_clear_queue(dnbd, &dnbd->tx_queue);
	}

	spin_unlock_irq(&dnbd->thread_lock);
}

/* send a request via network */
static int sock_xmit(dnbd_device_t * dnbd, int send, void *buf, int size,
		     int flags)
{
	int result = 0;
	struct msghdr msg;
	struct kvec iov;
	unsigned long irqflags;
	sigset_t oldset;

	/* do not allow signals, except of SIGKILL */
	spin_lock_irqsave(&current->sighand->siglock, irqflags);
	oldset = current->blocked;
	sigfillset(&current->blocked);
	sigdelsetmask(&current->blocked, sigmask(SIGKILL));
	recalc_sigpending();
	spin_unlock_irqrestore(&current->sighand->siglock, irqflags);

	/* prepare data structures and call kernel send routine */
	do {
		dnbd->sock->sk->sk_allocation = GFP_NOIO;
		iov.iov_base = buf;
		iov.iov_len = size;
		if (send) {
			msg.msg_name = &dnbd->mcast;
			msg.msg_namelen = sizeof(dnbd->mcast);
		} else {
			msg.msg_name = NULL;
			msg.msg_namelen = 0;
		}
		msg.msg_control = NULL;
		msg.msg_controllen = 0;
		msg.msg_flags = MSG_NOSIGNAL;

		if (send) {
			result =
			    kernel_sendmsg(dnbd->sock, &msg, &iov, 1,
					   size);
		} else {
			result =
			    kernel_recvmsg(dnbd->sock, &msg, &iov, 1, size,
					   0);
		}

		if (result <= 0)
			break;

		size -= result;
		buf += result;

	} while (0);

	/* set signal mask to original state */
	spin_lock_irqsave(&current->sighand->siglock, irqflags);
	current->blocked = oldset;
	recalc_sigpending();
	spin_unlock_irqrestore(&current->sighand->siglock, irqflags);

	return result;
}

/* copy sectors to cache */
static void dnbd_xfer_to_cache(dnbd_device_t * dnbd, struct sk_buff *skb,
			       int offset, int remain, sector_t sector)
{
	mm_segment_t oldfs = get_fs();
	int result;
	size_t blksize = dnbd->cache.blksize;
	char block_buf[blksize];
	struct iovec iov;

	if (!dnbd->cache.active)
		return;

	set_fs(get_ds());
	while (remain >= blksize) {
		iov.iov_base = &block_buf;
		iov.iov_len = blksize;
		/* copy data from socket buffer */
		if ((result =
		     skb_copy_datagram_iovec(skb, offset, &iov,
					     blksize)) < 0) {
			printk(KERN_WARNING
			       "dnbd: error copy packet to iovec!\n");
		}
		/* and insert to cache */
		dnbd->cache.insert(&dnbd->cache, sector, &block_buf);
		remain -= blksize;
		offset += blksize;
		sector += blksize / (1 << 9);
	}
	set_fs(oldfs);
}

/* process incoming network packets */
static int inline dnbd_recv_reply(dnbd_device_t * dnbd)
{
	mm_segment_t oldfs = get_fs();
	int i;
	unsigned int nsect = 0;
	int err;
	struct sk_buff *skb;
	struct iovec iov;
	int remain, offset, tocopy;
	dnbd_reply_t *reply;
	struct request *req = NULL;
	struct bio *bio;
	struct bio_vec *bvec;
	int tt;
	void *kaddr;

	/* sleep until packet arrives */
	skb = skb_recv_datagram(dnbd->sock->sk, 0, 0, &err);

	if (!skb)
		goto out_nofree;

	/* 
	   some NICs can verify checksums themselves and then is 
	   unnecessary for us 
	 */
	offset = sizeof(struct udphdr);
	if (skb->ip_summed != CHECKSUM_UNNECESSARY && (unsigned short)
	    csum_fold(skb_checksum(skb, 0, skb->len, skb->csum))) {
		printk(KERN_ERR "dnbd: udp checksum error!\n");
		goto out;
	}
	reply = (dnbd_reply_t *) (skb->data + offset);

	/* transform values from network to host byte order */
	reply->magic = ntohl(reply->magic);
	reply->id = ntohs(reply->id);
	reply->time = ntohs(reply->time);
	reply->cmd = ntohs(reply->cmd);
	reply->pos = be64_to_cpu(reply->pos);

	if (reply->magic != DNBD_MAGIC) {
		printk(KERN_ERR "dnbd: wrong magic in reply!\n");
		goto out;
	}

	/* calculate RTT */
	tt = jiffies & 0xffff;
	tt -= reply->time;
	if (tt < 0)
		tt += 1 << 16;

	/* check reply command */
	if (reply->cmd & DNBD_CMD_SRV) {
		switch (reply->cmd & DNBD_CMD_MASK) {
		case DNBD_CMD_READ:
			break;
		case DNBD_CMD_HB:
			if (!dnbd_set_serverid(&dnbd->servers, reply->id))
				printk(KERN_INFO
				       "dnbd: (re)activate server #%i\n",
				       reply->id);
			/* update times */
			dnbd_rx_update(dnbd->servers, reply->id);
			dnbd_rtt_server(&dnbd->servers, reply->id, tt);
		default:
			goto out;
		}
	} else
		goto out;

	/* update times */
	dnbd_rx_update(dnbd->servers, reply->id);

	/* try to find outstanding request */
	req = dnbd_deq_request_handle(&dnbd->rx_queue, reply->pos);

	offset += sizeof(struct dnbd_reply);
	remain = skb->len - offset;

	/* we know this request? No? Let's cache it ... */
	if (!req) {
		if ((reply->cmd & DNBD_CMD_SRV)
		    && (reply->cmd & DNBD_CMD_READ))
			dnbd_xfer_to_cache(dnbd, skb, offset, remain,
					   reply->pos >> 9);
		if (!req)
			goto out;
	}

	/* the reply fits to an outstanding request */
	dnbd_rtt_server(&dnbd->servers, reply->id, tt);

	nsect = 0;
	err = 0;
	/* copy network data to BIOs */
	rq_for_each_bio(bio, req) {
		bio_for_each_segment(bvec, bio, i) {
			tocopy = bvec->bv_len;
			if (tocopy > remain)
				goto nobytesleft;
			kaddr = kmap(bvec->bv_page);
			iov.iov_base = kaddr + bvec->bv_offset;
			iov.iov_len = tocopy;
			set_fs(KERNEL_DS);
			err =
			    skb_copy_datagram_iovec(skb, offset, &iov,
						    tocopy);
			set_fs(oldfs);
			kunmap(bvec->bv_page);

			if (err) {
				printk(KERN_ERR "dnbd: ERROR copy data\n");
				goto nobytesleft;
			}

			offset += tocopy;
			remain -= tocopy;
			nsect += bvec->bv_len >> 9;
		}
	}
      nobytesleft:
	/* end request partially or fully */
	if (dnbd_end_request(dnbd, req, 1, nsect)) {
		dnbd_enq_request(&dnbd->tx_queue, req, 1);
	}
      out:
	/* free reserved memory of packet */
	skb_free_datagram(dnbd->sock->sk, skb);
      out_nofree:

	return nsect;
}

static int dnbd_send_request(dnbd_device_t * dnbd, struct request *req)
{
	int result = 0;
	dnbd_request_t request;
	unsigned long size = req->current_nr_sectors << 9;
	int id;

	/* find nearest server */
	id = dnbd_next_server(&dnbd->servers);

	/* fill structure for a DNBD request */
	request.magic = cpu_to_be32(DNBD_MAGIC);
	request.id = cpu_to_be16((u16) id);
	request.time = cpu_to_be16(jiffies & 0xffff);
	request.cmd = cpu_to_be16(DNBD_CMD_READ | DNBD_CMD_CLI);
	request.pos = cpu_to_be64((u64) req->sector << 9);
	request.len = cpu_to_be16(size);

	/* send DNBD request */
	INFO("Sending request of %d sectors, starting from sector no: %llu\n", 
			request.len, request.pos);
	result = sock_xmit(dnbd, 1, &request, sizeof(request), 0);
	/* set times */
	req->start_time = jiffies;
	dnbd_tx_update(dnbd->servers, id);

	return result;
}

/* same for heartbeats */
static int dnbd_send_hb(dnbd_device_t * dnbd)
{
	int result = 0;
	dnbd_request_t request;

	request.magic = cpu_to_be32(DNBD_MAGIC);
	request.id = cpu_to_be16((u16) 0);
	request.time = cpu_to_be16(jiffies & 0xffff);
	request.cmd = cpu_to_be16(DNBD_CMD_HB | DNBD_CMD_CLI);
	request.pos = 0;
	request.len = 0;

	INFO("Sending heartbeat command \n");
	result = sock_xmit(dnbd, 1, &request, sizeof(request), 0);

	return result;
}

/* helper function to start threads */
static int dnbd_start_thread(dnbd_device_t * dnbd,
			     dnbd_thread_t * thread, thread_fn_t fn)
{
	int result = -EINVAL;
	struct task_struct *task;

	spin_lock(&dnbd->thread_lock);

	task = thread->task;
	if (!task)
		thread->task = (struct task_struct *) -1;

	spin_unlock(&dnbd->thread_lock);

	if (task)
		return -EBUSY;

	init_completion(&thread->startup);
	init_completion(&thread->finish);

	result = kernel_thread(fn, dnbd, CLONE_FS | CLONE_FILES);

	if (result)
		wait_for_completion(&thread->startup);
	return result;
}

/* helper function to stop threads */
static int dnbd_stop_thread(dnbd_device_t * dnbd,
			    dnbd_thread_t * thread, int wait)
{
	pid_t signaled;
	struct task_struct *task;

	signaled = 0;
	spin_lock(&dnbd->thread_lock);
	task = thread->task;
	if (task) {
		force_sig(SIGKILL, task);
		signaled = task->pid;
	}
	spin_unlock(&dnbd->thread_lock);
	if (signaled) {
		if (wait)
			wait_for_completion(&thread->finish);
		return 1;
	}
	return 0;
}

/* helper function for clean up */
static void dnbd_end_io(dnbd_device_t * dnbd)
{
	dnbd_clear_queues(dnbd);
	wake_up(&dnbd->io_waiters);
}

/* rx_loop function */
static int dnbd_rx_loop(void *data)
{
	int signr;

	dnbd_device_t *dnbd = (dnbd_device_t *) data;

	__module_get(THIS_MODULE);
	printk("rx_loop: enter\n");
	atomic_inc(&dnbd->num_io_threads);
	daemonize("dnbd_rx_loop");
	allow_signal(SIGKILL);

	spin_lock(&dnbd->thread_lock);
	dnbd->rx_thread.task = current;
	spin_unlock(&dnbd->thread_lock);

	complete(&dnbd->rx_thread.startup);

	/* loop until SIGKILL arrives */
	while ((signr = signal_pending(current)) == 0) {
		dnbd_recv_reply(dnbd);
	}

	spin_lock(&dnbd->thread_lock);
	dnbd->rx_thread.task = NULL;
	spin_unlock(&dnbd->thread_lock);

	dnbd_stop_thread(dnbd, &dnbd->rx_thread, 0);
	complete(&dnbd->rx_thread.finish);
	if (atomic_dec_and_test(&dnbd->num_io_threads))
		dnbd_end_io(dnbd);

	printk("rx_loop: leave\n");
	module_put(THIS_MODULE);

	return 0;
}

static int dnbd_tx_loop(void *data)
{
	int signr;
	dnbd_device_t *dnbd = (dnbd_device_t *) data;
	struct request *req;
	int result, cached;

	__module_get(THIS_MODULE);
	printk("tx_loop: enter\n");
	atomic_inc(&dnbd->num_io_threads);
	daemonize("dnbd_tx_loop");
	allow_signal(SIGKILL);

	spin_lock(&dnbd->thread_lock);
	dnbd->tx_thread.task = current;
	spin_unlock(&dnbd->thread_lock);

	complete(&dnbd->tx_thread.startup);

	/* loop until SIGKILL arrives */
	while ((signr = signal_pending(current)) == 0) {
		req = dnbd_try_deq_request(&dnbd->tx_queue);

		if (!req)
			continue;

		/* request already in cache? */
		cached = dnbd->cache.search(&dnbd->cache, req);

		if (cached) {
			if (!end_that_request_first(req, 1, cached)) {
#if LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,16)
				end_that_request_last(req,1);
#else
				end_that_request_last(req);
#endif
			} else {
				dnbd_enq_request(&dnbd->tx_queue, req, 1);
			}
			continue;
		}

		dnbd_enq_request(&dnbd->rx_queue, req, 0);
		result = dnbd_send_request(dnbd, req);
	}

	spin_lock(&dnbd->thread_lock);
	dnbd->tx_thread.task = NULL;
	spin_unlock(&dnbd->thread_lock);

	dnbd_stop_thread(dnbd, &dnbd->tx_thread, 0);
	complete(&dnbd->tx_thread.finish);
	if (atomic_dec_and_test(&dnbd->num_io_threads))
		dnbd_end_io(dnbd);

	printk("tx_loop: leave\n");
	module_put(THIS_MODULE);
	return 0;
}

/* rexmit function is called periodically by kernel timer */
static void dnbd_rexmit(unsigned long arg)
{
	dnbd_device_t *dnbd = (dnbd_device_t *) arg;
	unsigned long flags;
	unsigned long timeout;

	int requeued;

	/* difference in jiffies for request timeout */
	int diff = dnbd->servers.asrtt >> SRTT_SHIFT;

	/* just in case, give boundaries for request timeouts */
	if (diff < dnbd->servers.timeout_min)
		diff = dnbd->servers.timeout_min;
	if (diff > dnbd->servers.timeout_max)
		diff = dnbd->servers.timeout_max;

	timeout = jiffies - (diff << TIMEOUT_SHIFT);

	requeued =
	    dnbd_requeue_requests(&dnbd->tx_queue, &dnbd->rx_queue,
				  timeout);

	/* set timer again in ASRTT jiffies for better granularity */
	if (dnbd->state & DNBD_STATE_RUNNING) {
		spin_lock_irqsave(&dnbd->timer_lock, flags);
		dnbd->timer.expires = jiffies + diff;
		add_timer(&dnbd->timer);
		spin_unlock_irqrestore(&dnbd->timer_lock, flags);
	}
}

/* session loop takes care of statistics */
static int dnbd_ss_loop(void *data)
{
	dnbd_device_t *dnbd = (dnbd_device_t *) data;
	int signr;

	__module_get(THIS_MODULE);
	printk("ss_loop: enter\n");
	atomic_inc(&dnbd->num_io_threads);
	daemonize("dnbd_ss_loop");
	allow_signal(SIGKILL);

	spin_lock(&dnbd->thread_lock);
	dnbd->ss_thread.task = current;
	spin_unlock(&dnbd->thread_lock);

	complete(&dnbd->ss_thread.startup);

	while ((signr = signal_pending(current)) == 0) {
		set_current_state(TASK_INTERRUPTIBLE);
		schedule_timeout(HZ * 4);	/* fixme: give user space option */
		set_current_state(TASK_RUNNING);
		dnbd_servers_weight(&dnbd->servers);
		dnbd_send_hb(dnbd);
	}

	spin_lock(&dnbd->thread_lock);
	dnbd->ss_thread.task = NULL;
	spin_unlock(&dnbd->thread_lock);

	dnbd_stop_thread(dnbd, &dnbd->ss_thread, 0);
	complete(&dnbd->ss_thread.finish);
	if (atomic_dec_and_test(&dnbd->num_io_threads))
		dnbd_end_io(dnbd);

	printk("ss_loop: leave\n");
	module_put(THIS_MODULE);
	return 0;

}

/* waits until a thread has exited */
static int dnbd_wait_threads_finished(dnbd_device_t * dnbd)
{
	int signaled = 0;
	struct task_struct *tsk = current;
	DECLARE_WAITQUEUE(wait, tsk);

	if (atomic_read(&dnbd->num_io_threads) > 0) {
		add_wait_queue(&dnbd->io_waiters, &wait);
		set_current_state(TASK_INTERRUPTIBLE);
		if (atomic_read(&dnbd->num_io_threads) > 0)
			schedule();
		set_current_state(TASK_RUNNING);
		if (signal_pending(current))
			signaled = 1;
		remove_wait_queue(&dnbd->io_waiters, &wait);
	}
	return signaled ? 0 : 1;
}

/* activate threads (rx_loop, tx_loop, ss_loop) */
static int dnbd_activate_threads(dnbd_device_t * dnbd)
{
	int result = -EINVAL;

	printk(KERN_NOTICE "dnbd: activating threads...\n");
	result = dnbd_start_thread(dnbd, &dnbd->rx_thread, dnbd_rx_loop);
	if (result < 0)
		return result;

	result = dnbd_start_thread(dnbd, &dnbd->tx_thread, dnbd_tx_loop);
	if (result < 0) {
		dnbd_stop_thread(dnbd, &dnbd->rx_thread, 1);
		return result;
	}
	result = dnbd_start_thread(dnbd, &dnbd->ss_thread, dnbd_ss_loop);
	if (result < 0) {
		dnbd_stop_thread(dnbd, &dnbd->rx_thread, 1);
		dnbd_stop_thread(dnbd, &dnbd->tx_thread, 1);
		return result;
	}
	return 0;
}

/* deactivate threads (rx_loop, tx_loop, ss_loop) */
static int dnbd_deactivate_threads(dnbd_device_t * dnbd)
{

	printk(KERN_NOTICE "dnbd: deactivating threads...\n");
	dnbd_stop_thread(dnbd, &dnbd->tx_thread, 1);
	dnbd_stop_thread(dnbd, &dnbd->rx_thread, 1);
	dnbd_stop_thread(dnbd, &dnbd->ss_thread, 1);
	return 0;
}

/* start threads and activate timer for retransmits */
static int dnbd_startup(dnbd_device_t * dnbd)
{
	int result = -EINVAL;
	result = dnbd_activate_threads(dnbd);

	if (result < 0) {
		printk(KERN_NOTICE
		       "dnbd_startup: ERROR activating threads!\n");

		goto out;
	}

	dnbd->state = DNBD_STATE_RUNNING;

	dnbd->timer.data = (unsigned long) dnbd;
	dnbd->timer.function = dnbd_rexmit;
	dnbd->timer.expires = jiffies;
	add_timer(&dnbd->timer);
      out:
	return result;
}

/* disable timer and shutdown threads */
static int dnbd_shutdown(dnbd_device_t * dnbd)
{
	int result = -EINVAL;
	del_timer(&dnbd->timer);
	result = dnbd_deactivate_threads(dnbd);
	if (result < 0)
		printk(KERN_NOTICE
		       "dnbd_shutdown: ERROR deactivating threads!\n");
	else
		dnbd->state &= ~DNBD_STATE_RUNNING;

	return result;
}

/* startup with semaphore */
static int dnbd_start(dnbd_device_t * dnbd)
{
	int result;

	down(&dnbd->semalock);
	result = dnbd_startup(dnbd);
	up(&dnbd->semalock);
	return result;
}

/* shutdown with semaphore */
static int dnbd_stop(dnbd_device_t * dnbd)
{
	int result;
	down(&dnbd->semalock);
	result = dnbd_shutdown(dnbd);
	up(&dnbd->semalock);
	return result;
}

/* function called by the kernel to make DNBD process a request */
static void dnbd_do_request(request_queue_t * q)
{
	dnbd_device_t *dnbd = NULL;
	int minor;

	struct request *req;

	/* as long as there are requests... */
	while ((req = elv_next_request(q)) != NULL) {

		/* dequeue request from kernel queue */
		blkdev_dequeue_request(req);
		if (!blk_fs_request(req)) {
			printk(KERN_NOTICE "Skip non-CMD request\n");
			goto error_out;
		}

		dnbd = req->rq_disk->private_data;
		if (!dnbd) {
			printk(KERN_ERR "dnbd: no private data\n");
		}

		minor = DEVICE_TO_MINOR(dnbd);

		if (!(dnbd->state & DNBD_STATE_RUNNING))
			goto error_out;

		if (rq_data_dir(req) != READ) {
			goto error_out;
		}

		/* 
		   enqueue request to tx_queue, where it will be fetched
		   by the tx_loop 
		 */
		spin_unlock_irq(q->queue_lock);
		dnbd_enq_request(&dnbd->tx_queue, req, 1);
		spin_lock_irq(q->queue_lock);

		continue;

	      error_out:
		spin_unlock_irq(q->queue_lock);
		dnbd_end_request(dnbd, req, 0, req->nr_sectors);
		spin_lock_irq(q->queue_lock);
	}
	return;
}

/* called from ioctl to set socket */
static int dnbd_set_sock(dnbd_device_t * dnbd, unsigned long arg)
{
	int result = -EINVAL;
	struct file *file = NULL;
	struct inode *inode = NULL;
	struct socket *sock = NULL;

	if (dnbd->sock || dnbd->file) {
		result = -EBUSY;
		goto out;
	}

	file = fget(arg);
	if (!file) {
		result = -EBADF;
		goto out;
	}

	inode = file->f_dentry->d_inode;
	if (!S_ISSOCK(inode->i_mode)) {
		result = -ENOTSOCK;
		goto out;
	}

	if (!(sock = SOCKET_I(inode))) {
		result = -ENOTSOCK;
		goto out;
	}

	if (sock->type != SOCK_DGRAM) {
		result = -EPROTONOSUPPORT;
		goto out;
	}

	atomic_inc(&dnbd->refcnt);
	dnbd->file = file;
	dnbd->sock = sock;

	result = 0;

      out:
	if (result < 0 && file)
		result = -EINVAL;

	return result;
}

/* release socket */
static int dnbd_clear_sock(dnbd_device_t * dnbd)
{
	int result = -EINVAL;
	struct file *file = NULL;
	struct socket *sock = NULL;

	if (!dnbd)
		goto out;

	spin_lock(&dnbd->thread_lock);
	sock = dnbd->sock;
	file = dnbd->file;
	dnbd->sock = NULL;
	dnbd->file = NULL;
	spin_unlock(&dnbd->thread_lock);

	if (!sock) {
		result = -ENOTCONN;
		goto out;
	}

	if (!file) {
		result = -EINVAL;
		goto out;
	}
	/* 
	 * space for operations when socket has to be cleared,
	 * which is done from user space (client/client.c)
	 */

	atomic_dec(&dnbd->refcnt);
	result = 0;

      out:
	if (file) {
		fput(file);
	}
	return result;

}

/* function is invoked from user space to start session */
static int dnbd_do_it(dnbd_device_t * dnbd)
{
	int result = 0;

	if (!try_module_get(THIS_MODULE)) {
		printk(KERN_ERR
		       "dnbd_do_it: try_module_get not worked!\n");
		goto out;
	}

	result = dnbd_start(dnbd);

	if (result < 0)
		goto out;

	/* 
	 * will return when session ends (disconnect), which is 
	 * invoked from user space 
	 */
	dnbd_wait_threads_finished(dnbd);

	dnbd_stop(dnbd);


	module_put(THIS_MODULE);

      out:
	return result;
}

static int dnbd_disconnect(dnbd_device_t * dnbd)
{
	int result = -EINVAL;

	if (!dnbd->sock) {
		result = -ENOTCONN;
		goto out;
	}

	/* end session and stop threads */
	dnbd_shutdown(dnbd);

	/* wait until threads exited */
	dnbd_wait_threads_finished(dnbd);

	/* clean up */
	dnbd_clear_sock(dnbd);
	dnbd->cache.clean(&dnbd->cache);
	dnbd_clean_servers(&dnbd->servers);

	result = 0;
      out:
	return result;

}

/* handle ioctl calls from user space */
static int dnbd_ioctl(struct inode *inode, struct file *file,
		      unsigned int cmd, unsigned long arg)
{
	int result = -EINVAL;
	dnbd_device_t *dnbd;
	int minor;

	if (!capable(CAP_SYS_ADMIN))
		return -EPERM;
	if (!inode)
		return -EINVAL;

	dnbd = inode->i_bdev->bd_disk->private_data;
	minor = DEVICE_TO_MINOR(dnbd);

	if (minor >= MAX_DNBD)
		return -ENODEV;

	/* different locking behavior needed for ioctl calls */
	switch (cmd) {
	case DNBD_DO_IT:
		return dnbd_do_it(dnbd);
	case DNBD_DISCONNECT:
		return dnbd_disconnect(dnbd);
	}

	down(&dnbd->semalock);
	switch (cmd) {
	case DNBD_SET_SOCK:
		result = dnbd_set_sock(dnbd, arg);
		break;
	case DNBD_SET_GROUPNET:
		result =
		    copy_from_user(&dnbd->mcast, (void *) arg,
				   sizeof(dnbd->mcast)) ? -EFAULT : 0;
		break;
	case DNBD_SET_BLKSIZE:
		dnbd->blksize = arg;
		printk(KERN_INFO "dnbd: setting blksize to %i\n",
		       dnbd->blksize);
		dnbd->bytesize &= ~(dnbd->blksize - 1);
		inode->i_bdev->bd_inode->i_size = dnbd->bytesize;
		set_blocksize(inode->i_bdev, dnbd->blksize);
		set_capacity(dnbd->disk, dnbd->bytesize >> 9);
		result = 0;
		break;
	case DNBD_SET_CAPACITY:
		result =
		    copy_from_user(&dnbd->bytesize, (void *) arg,
				   sizeof(dnbd->bytesize)) ? -EFAULT : 0;
		if (result)
			break;
		dnbd->bytesize = dnbd->bytesize & ~(dnbd->blksize - 1);
		inode->i_bdev->bd_inode->i_size = dnbd->bytesize;
		set_blocksize(inode->i_bdev, dnbd->blksize);
		set_capacity(dnbd->disk, dnbd->bytesize >> 9);
		result = 0;
		break;
	case DNBD_SET_CACHE:
		result =
		    dnbd_cache_set(&dnbd->cache,
				   (struct dnbd_file __user *) arg,
				   inode->i_bdev->bd_block_size);
		break;
	case DNBD_SET_SERVERID:
		result = dnbd_set_serverid(&dnbd->servers, arg);
		break;
	default:
		result = -EINVAL;
	}
	up(&dnbd->semalock);

	return result;
}

static int dnbd_open(struct inode *inode, struct file *file)
{
	int result = -EINVAL;
	dnbd_device_t *dnbd;
	int minor;

	if (!inode)
		return -EINVAL;

	dnbd = inode->i_bdev->bd_disk->private_data;
	minor = DEVICE_TO_MINOR(dnbd);

	if (minor >= MAX_DNBD)
		return -ENODEV;

	result = 0;
	down(&dnbd->semalock);

	/* open only read-only */
	if ((file->f_mode & FMODE_WRITE)) {
		result = -EROFS;
		goto out;
	}

	/* increment reference counter */
	atomic_inc(&dnbd->refcnt);
      out:
	up(&dnbd->semalock);
	return result;
}

static int dnbd_release(struct inode *inode, struct file *file)
{
	dnbd_device_t *dnbd;
	int minor;

	if (!inode)
		return -EINVAL;

	dnbd = inode->i_bdev->bd_disk->private_data;
	minor = DEVICE_TO_MINOR(dnbd);

	if (minor >= MAX_DNBD)
		return -ENODEV;

	down(&dnbd->semalock);
	
	/* decrement reference counter */
	atomic_dec(&dnbd->refcnt);

	up(&dnbd->semalock);
	return 0;
}

static struct block_device_operations dnbd_fops = {
	.ioctl = dnbd_ioctl,
	.open = dnbd_open,
/*	.owner = THIS_MODULE, */
	.release = dnbd_release,
};

/* reader function for proc interface */
static int
dnbd_read_proc(char *buf, char **start, off_t offset,
	       int count, int *eof, void *data)
{
	int i, len = 0;
	dnbd_device_t *dnbd;

	i = (int) data;
	dnbd = dnbd_dev[i];


	spin_lock(&dnbd->thread_lock);

	len +=
	    snprintf(buf + len, count - len,
		     "Cache:\n hits %li\n miss %li\n lru replaced %li\n",
		     dnbd->cache.hits, dnbd->cache.miss, dnbd->cache.lru);

	len += snprintf(buf + len, count - len, "Servers:\n");

	len += dnbd_show_servers(&dnbd->servers, buf + len, count - len);

	spin_unlock(&dnbd->thread_lock);

	*eof = 1;
	return len;
}

/* register network block device */
static int __init dnbd_init(void)
{
	int err = -ENOMEM;
	int i = 0;
	char name[] = "dnbdxx";

	if (!(dnbd_proc_dir = proc_mkdir("driver/dnbd", NULL))) {
		printk(KERN_ERR
		       "dnbd: can't create dir /proc/driver/dnbd\n");
		goto out;
	}

	for (i = 0; (i < MAX_DNBD && i < 100); i++) {
		dnbd_dev[i] = vmalloc(sizeof(dnbd_device_t));
			if(!dnbd_dev[i]) {
				printk(KERN_ERR "dnbd%d: Not enough memory\n",i);
				goto out;
			} else {
				memset(dnbd_dev[i],0,sizeof(dnbd_device_t));
			}
	}
	for (i = 0; (i < MAX_DNBD && i < 100); i++) {
		sprintf(name, "dnbd%i", i);
		if (!create_proc_read_entry
		    (name, 0, dnbd_proc_dir, dnbd_read_proc, (void *) i)) {
			printk(KERN_ERR
			       "dnbd: can't create /proc/driver/dnbd\n");
			goto out;
		}
	}


	for (i = 0; i < MAX_DNBD; i++) {
		/* 
		 * get pre initialized structure for block device minor
		 */
		struct gendisk *disk = alloc_disk(1);
		if (!disk) {
			printk(KERN_CRIT "dnbd: alloc_disk failed\n");
			goto out;
		}
		dnbd_dev[i]->disk = disk;
		/* 
		 * initizialisation of request queue 
		 * dnbd_do_request() is our function to handle the requests
		 */
		disk->queue =
		    blk_init_queue(dnbd_do_request,
				   &dnbd_dev[i]->thread_lock);

		if (!disk->queue) {
			printk(KERN_CRIT "dnbd: blk_init_queue failed\n");
			put_disk(disk);
			goto out;
		}

		/* read ahead */
		disk->queue->backing_dev_info.ra_pages = 8;

	}

	/* unregister_blkdev(DNBD_MAJOR, "dnbd"); */
	if ((dnbd_major = register_blkdev(DNBD_MAJOR, "dnbd")) < 0) {
		printk(KERN_CRIT "dnbd: register_blkdev failed\n");
		err = -EIO;
		goto out;
	}

	printk(KERN_INFO "dnbd: module loaded with major %i\n",
	       dnbd_major);

	devfs_mk_dir("dnbd");
	for (i = 0; i < MAX_DNBD; i++) {
		struct gendisk *disk = dnbd_dev[i]->disk;
		dnbd_dev[i]->state = DNBD_STATE_LOADED;
		init_MUTEX(&dnbd_dev[i]->semalock);
		init_timer(&dnbd_dev[i]->timer);

		spin_lock_init(&dnbd_dev[i]->thread_lock);
		spin_lock_init(&dnbd_dev[i]->queue_lock);
		spin_lock_init(&dnbd_dev[i]->timer_lock);

		/* initialize up rx&tx queue */
		dnbd_dev[i]->rx_thread.task = NULL;
		dnbd_dev[i]->tx_thread.task = NULL;
		atomic_set(&dnbd_dev[i]->num_io_threads, 0);
		init_waitqueue_head(&dnbd_dev[i]->io_waiters);
		spin_lock_init(&dnbd_dev[i]->rx_queue.lock);
		INIT_LIST_HEAD(&dnbd_dev[i]->rx_queue.head);
		init_waitqueue_head(&dnbd_dev[i]->rx_queue.waiters);
		spin_lock_init(&dnbd_dev[i]->tx_queue.lock);
		INIT_LIST_HEAD(&dnbd_dev[i]->tx_queue.head);
		init_waitqueue_head(&dnbd_dev[i]->tx_queue.waiters);

		/* initialize device characteristics */
		dnbd_dev[i]->file = NULL;
		dnbd_dev[i]->magic = LO_MAGIC;
		dnbd_dev[i]->blksize = 1 << 9;
		dnbd_dev[i]->bytesize = 0;
		disk->major = dnbd_major;
		disk->first_minor = i;
		disk->fops = &dnbd_fops;
		disk->private_data = &dnbd_dev[i];
		disk->flags |= GENHD_FL_SUPPRESS_PARTITION_INFO;
		sprintf(disk->disk_name, "dnbd%d", i);
		sprintf(disk->devfs_name, "dnbd/%d", i);
		set_capacity(disk, 0);

		/* initialize cache */
		dnbd_cache_init(&dnbd_dev[i]->cache);

		/* initialize servers */
		dnbd_servers_init(&dnbd_dev[i]->servers);

		/* register disk to kernel */
		add_disk(disk);
	}

	return 0;

      out:
	printk(KERN_CRIT "dnbd: could not initialize dnbd!\n");
	while (i--) {
		blk_cleanup_queue(dnbd_dev[i]->disk->queue);
		put_disk(dnbd_dev[i]->disk);
	}
	return err;
}

/* unregister network block device */
static void __exit dnbd_exit(void)
{
	int i;
	char name[] = "dnbdxx";
	struct gendisk *disk;

	/* force disconnects */
	for (i = 0; i < MAX_DNBD; i++) {
		if (!dnbd_disconnect(dnbd_dev[i])) {
			printk(KERN_INFO "dnbd%i: disconnected.\n", i);
		}
	}

	/* remove disks */
	for (i = 0; i < MAX_DNBD; i++) {
		dnbd_rem_servers(&dnbd_dev[i]->servers);

		disk = dnbd_dev[i]->disk;
		if (disk) {
			del_gendisk(disk);
			blk_cleanup_queue(disk->queue);
			put_disk(disk);
		}
	}
	devfs_remove("dnbd");
	unregister_blkdev(dnbd_major, "dnbd");

	for (i = 0; (i < MAX_DNBD && i < 100); i++) {
		sprintf(name, "dnbd%i", i);
		remove_proc_entry(name, dnbd_proc_dir);
		vfree(dnbd_dev[i]);
	}

	remove_proc_entry("driver/dnbd", NULL);

	printk(KERN_INFO "dnbd: unregistered device.\n");

}

module_init(dnbd_init);
module_exit(dnbd_exit);

MODULE_DESCRIPTION("Distributed Network Block Device");
MODULE_LICENSE("GPL");
