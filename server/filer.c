/*
 * filer.c - open, seeks in and reads from a file 
 * 
 * Copyright (C) 2006 Thorsten Zitterell <thorsten@zitterell.de>
 */
 
#include <stdlib.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <pthread.h>

#include "filer.h"

/* 
 * function filer_getcapacity()
 * returns: size/capacity of file/device 
 */
unsigned long long filer_getcapacity(filer_info_t * filer_info)
{
	return filer_info->size;
}

/* 
 * function filer_seekblock(): seek to position in file/block device 
 * returns: 1 on success, otherwise 0
 */
static inline int filer_seekblock(filer_info_t * filer_info, off_t newpos)
{
	if (lseek(filer_info->fd, newpos, SEEK_SET) == (off_t) -1) {
		return 0;
	}
	filer_info->pos = newpos;
	return 1;
}

/* 
 * function filer_readblock(): read bytes at specific position 
 * returns: 1 on success, otherwise 0
 */
inline int filer_readblock(filer_info_t * filer_info, void *buf, size_t size,
		     off_t pos)
{
	size_t remain = size;
	int result = 0;
	int numblocks = 0;
	
	if (!filer_seekblock(filer_info, pos)) goto leave;		
	
	while (remain > 0) {
		if ((numblocks = read(filer_info->fd, buf, remain)) <= 0) {
			if (errno == EINTR)
				continue;
			goto leave;
		}
		
		if (numblocks == 0) {
			goto leave;
		}
		remain -= numblocks;
		buf += numblocks;
	}
	result = 1;
      leave:
	filer_info->pos += (size - remain);
	return result;
}

/* 
 * function filer_init(): open file to be served
 * returns: data structure with file information 
 */
filer_info_t *filer_init(const char *filename)
{
	filer_info_t *filer_info;
	struct stat64 stbuf;

	filer_info = (filer_info_t *) malloc(sizeof(filer_info_t));
	if (!filer_info)
		return NULL;

	filer_info->filename = strdup(filename);
	if ((filer_info->fd = open(filename, O_RDONLY | O_LARGEFILE)) < 0) {
		fprintf(stderr, "ERROR: Cannot open filename \"%s\"\n",
			filename);
		goto out_free;
	}

	stbuf.st_size = 0;
	
	if (fstat64(filer_info->fd, &stbuf) < 0) {
		fprintf(stderr, "ERROR: Cannot stat file \"%s\"\n",
			filename);
		goto out_free;
	}

	/* get file/device size */
	if ((filer_info->size = stbuf.st_size) == 0) {
		filer_info->size = lseek64(filer_info->fd, (off_t) 0, SEEK_END);
	}

	if (filer_info->size == 0) {
	    fprintf(stderr, "ERROR: File/device has zero size\n");
		goto out_free;
	}

	goto out;

      out_free:
	if (filer_info)
		free(filer_info);

	filer_info = NULL;
      out:
	return filer_info;
}
