#ifndef LINUX_DNBD_CLISERV_H
#define LINUX_DNBD_CLISERV_H	1

#ifndef MODULE
#include <stdint.h>
#endif

#ifdef DNBD_USERSPACE
#include <bits/types.h>
#include <netinet/in.h>
#include <endian.h>
#include <byteswap.h>

/* host byte order <-> network byte order */
#if __BYTE_ORDER == __BIG_ENDIAN
#define ntohll(x) (x)
#else
#define ntohll(x) bswap_64(x)
#endif

#else
#include <linux/in.h>
#endif

#include <linux/types.h>

/* some constants */
#define		DNBD_VERSION		"0.9.0"
#define		DNBD_PORT		5001
#define		DNBD_MAGIC		0x19051979
#define 	DNBD_MAJOR 		0
#define		DNBD_UIDLEN		20

/* states */
#define	DNBD_STATE_LOADED	1<<0
#define DNBD_STATE_CONFIGURED	1<<1
#define DNBD_STATE_RUNNING	1<<2

/* define ioctls */
#define DNBD_IOCTL_TYPE		0xac
#define DNBD_SET_SOCK		_IO( DNBD_IOCTL_TYPE, 0)
#define DNBD_SET_GROUPNET	_IO( DNBD_IOCTL_TYPE, 1)
#define DNBD_SET_BLKSIZE	_IO( DNBD_IOCTL_TYPE, 2)
#define DNBD_SET_CAPACITY	_IO( DNBD_IOCTL_TYPE, 3)
#define DNBD_SET_SERVERID	_IO( DNBD_IOCTL_TYPE, 4)
#define DNBD_SET_CACHE		_IO( DNBD_IOCTL_TYPE, 5)
#define DNBD_DO_IT		_IO( DNBD_IOCTL_TYPE, 6)
#define DNBD_DISCONNECT		_IO( DNBD_IOCTL_TYPE, 7)

/* define communication between server and client */
#define DNBD_CMD_MASK		0x07
#define DNBD_CMD_INIT		0x01
#define DNBD_CMD_READ		0x02
#define DNBD_CMD_HB		0x03

#define DNBD_CMD_CLI		0x08
#define DNBD_CMD_SRV		0x10

#define DNBD_TMR_OUT		0x0a

/* do not allign variables to 32bit etc.*/
#pragma pack(1) 
struct dnbd_request {
	uint32_t magic;
	uint16_t id;
	uint16_t cmd;
	uint64_t pos;
	uint16_t time;
	uint16_t len;
};
#pragma pack()

#pragma pack(1)
struct dnbd_reply {
	uint32_t magic;
	uint16_t id;
	uint16_t cmd;
	uint64_t pos;
	uint16_t time;
};
#pragma pack()

#pragma pack(1)
struct dnbd_reply_init {
	uint32_t magic;
	uint16_t id;
	uint16_t cmd;
	uint64_t capacity;
	uint16_t time;
	uint16_t blksize;	
};
#pragma pack()

typedef struct dnbd_reply dnbd_reply_t;
typedef struct dnbd_reply_init dnbd_reply_init_t;
typedef struct dnbd_request dnbd_request_t;
	
struct dnbd_file {
	const char *name;
	int len;
};

#endif /* LINUX_DNBD_CLISERV_H */
