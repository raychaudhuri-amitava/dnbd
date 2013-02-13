#ifndef LINUX_DNBD_FILER_H
#define LINUX_DNBD_FILER_H	1

/* information of served file/block device */
struct filer_info {
	const char *filename;
	int fd;
	unsigned long long size;
	off_t pos;
};

typedef struct filer_info filer_info_t;

/* functions */
unsigned long long filer_getcapacity(filer_info_t * filer);
int inline filer_readblock(filer_info_t * filer_info, void *buf, size_t size, off_t pos);
filer_info_t *filer_init(const char *filename);

#endif
