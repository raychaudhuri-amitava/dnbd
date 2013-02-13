#define DNBD_MAJOR 43
#define MAX_DNBD 32

struct dnbd_device {
	int flags;
	int harderror;		/* Code of hard error			*/
	struct socket * sock;
	struct file * file; 	/* If == NULL, device is not ready, yet	*/
	int magic;
	spinlock_t queue_lock;
	struct list_head queue_head;/* Requests are added here...	*/
	struct semaphore tx_lock;
	struct gendisk *disk;
	int blksize;
	u64 bytesize;
};

typedef struct dnbd_device dnbd_device_t;
