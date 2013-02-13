/*
 * cache.c - block cache with red-black trees
 * Copyright (C) 2006 Thorsten Zitterell <thorsten@zitterell.de>
 */

#include <linux/module.h>
#include <linux/types.h>
#include <linux/highmem.h>
#include <linux/fs.h>
#include <linux/file.h>
/* use red-black library of kernel */
#include <linux/rbtree.h>
#include <asm/uaccess.h>

#include "../common/dnbd-cliserv.h"
#include "cache.h"

/* empty node */
#define rb_entry_cn(node)	rb_entry((node), struct cache_node, rb_node)

/* dummy operations of no cache is used */
int dnbd_cache_dummy_search(dnbd_cache_t * cache, struct request *req)
{
	return 0;
}

int dnbd_cache_dummy_insert(dnbd_cache_t * cache, sector_t block,
			    void *buf)
{
	return 0;
}

void dnbd_cache_dummy_clean(dnbd_cache_t * cache)
{
	return;
}

int dnbd_cache_search(dnbd_cache_t * cache, struct request *req)
{
	/* hold segment as we copy from user space */
	mm_segment_t old_fs = get_fs();
	size_t blksize;
	void *kaddr;

	int i;
	struct bio *bio;
	struct bio_vec *bvec;

	int result = 0, rbytes;
	struct rb_node *n;
	struct cache_node *cn;
	loff_t offset;
	char *buf;

	down(&cache->sema);
	n = cache->root.rb_node;
	blksize = cache->blksize;

	/* search for block */
	while (n) {
		cn = rb_entry_cn(n);

		if (req->sector < cn->rb_key)
			n = n->rb_left;
		else if (req->sector > cn->rb_key)
			n = n->rb_right;
		else
			goto found;
	}
	/* block is not cached */
	cache->miss++;
	goto out;

	/* cached block was found */
      found:
	cache->hits++;
	offset = cn->rb_data * blksize;
	rq_for_each_bio(bio, req) {
		bio_for_each_segment(bvec, bio, i) {
			if (bvec->bv_len > blksize) {
				printk(KERN_WARNING
				       "bvec->bv_len greater than cache block size\n");
				goto out;
			}
			/* copy cached block from cache file */
			set_fs(get_ds());
			buf = req->buffer;
			kaddr = kmap(bvec->bv_page);
			rbytes =
			    vfs_read(cache->filp, kaddr + bvec->bv_offset,
				     bvec->bv_len, &offset);
			kunmap(bvec->bv_page);
			set_fs(old_fs);

			/* error on reading? */
			if (rbytes != bio_iovec(req->bio)->bv_len) {
				printk
				    ("dnbd: ERROR reading from cache!\n");
				result = 0;
				goto out;
			}

			result += rbytes;
			if (result == blksize)
				goto out;
		}
	}

      out:
	up(&cache->sema);

	/* return number of copied sectors */
	return result >> 9;
}

int dnbd_cache_insert(dnbd_cache_t * cache, sector_t sector, void *buf)
{
	mm_segment_t old_fs = get_fs();
	int result = 0;
	struct rb_node **p;
	struct rb_node *parent = NULL;
	cache_node_t *__cn, *cn;
	sector_t act_block;

	loff_t offset;

	down(&cache->sema);
	p = &cache->root.rb_node;

	/* red-black tree search and replacement in O(log n) */
	
	/* check if node was already inserted to cache and,
	   if necessary, do LRU replacement */
	
	while (*p) {
		parent = *p;
		__cn = rb_entry_cn(parent);
		if (sector < __cn->rb_key)
			p = &(*p)->rb_left;
		else if (sector > __cn->rb_key)
			p = &(*p)->rb_right;
		else {
			/* the sector was already added to cache */

			/* LRU replacement policy */
			if (__cn->prev)
				__cn->prev->next = __cn->next;
			else
				/* __cn is head - do nothing */
				goto no_lru;

			if (__cn->next)
				__cn->next->prev = __cn->prev;
			else
				/* __cn is tail - so set new one */
				cache->tail =
				    (__cn->prev ? __cn->prev : __cn);

			/* insert new node to head:
			   head of list has no predecessor, 
			   set previous node to NULL and next 
			   node to old head and set new head */

			__cn->prev = NULL;
			__cn->next = cache->head;

			if (cache->head)
				cache->head->prev = __cn;

			cache->head = __cn;

			cache->lru++;
		      no_lru:
			result = 1;
			goto out;
		}
	}

	/* check if cache is full */
	if (cache->used_blocks == cache->max_blocks) {
		/* 
		   remove LRU node (cn), but keep reserved 
		   data structure in memory 
		*/
		cn = cache->tail;
		cache->tail->prev->next = NULL;
		cache->tail = cache->tail->prev;

		/*
		   Node (which is tail of LRU list) will be erased from tree 
		   which is then rebalanced. 
		   Re-finding a parent for the a node is mandatory.
		 */
		act_block = cn->rb_data;
		rb_erase(&cn->rb_node, &cache->root);
		p = &cache->root.rb_node;
		while (*p) {
			parent = *p;
			__cn = rb_entry_cn(parent);
			if (sector < __cn->rb_key)
				p = &(*p)->rb_left;
			else
				p = &(*p)->rb_right;
		}
	} else {
		/* cache is not full, so reserve memory for red-black tree node */
		if (!
		    (cn =
		     (cache_node_t *) kmalloc(sizeof(cache_node_t),
					      GFP_KERNEL))) {
			result = -ENOMEM;
			goto out;
		}
		act_block = cache->used_blocks;
		/* cn = &cache->nodes[act_block]; */
		cache->used_blocks++;
	}
	
	/* write block to cache file */	
	offset = act_block * cache->blksize;
	set_fs(get_ds());
	result = vfs_write(cache->filp, buf, cache->blksize, &offset);
	set_fs(old_fs);

	if (result != cache->blksize) {
		printk("dnbd: ERROR writing to cache!\n");
		cache->used_blocks--;
		kfree(cn);
		goto out;
	}
	
	/* cn (current node) points to an empty node, now */
	cn->rb_key = sector;
	cn->rb_data = act_block;

	/* current node (which will become the new head) has no predecessor */
	cn->prev = NULL;
	cn->next = cache->head;

	/* adapt head element - if it exists */
	if (cache->head)
		cache->head->prev = cn;

	/* set new head */
	cache->head = cn;

	/* set new tail */
	if (!cache->tail)
		cache->tail = cn;

	/* call kernel helpers for red-black trees */
	rb_link_node(&cn->rb_node, parent, p);
	rb_insert_color(&cn->rb_node, &cache->root);

      out:
	up(&cache->sema);
	return result;
}

int dnbd_cache_init(dnbd_cache_t * cache)
{
	int result = -EINVAL;

	/* initialize cache */
	cache->active = 0;
	
	/* set dummy function, if no cache is used */
	cache->insert = &dnbd_cache_dummy_insert;
	cache->search = &dnbd_cache_dummy_search;
	cache->clean = &dnbd_cache_dummy_clean;

	cache->root.rb_node = NULL;

	cache->max_blocks = 0;
	cache->used_blocks = 0;
	cache->blksize = 0;

	cache->hits = 0;
	cache->miss = 0;
	cache->lru = 0;

	cache->filp = NULL;
	cache->fname = NULL;

	cache->head = NULL;
	cache->tail = NULL;
	spin_lock_init(&cache->lock);
	init_MUTEX(&cache->sema);

	result = 0;
	return result;
}

void dnbd_cache_clean(dnbd_cache_t * cache)
{
	cache_node_t *node;
	cache_node_t *tmp;
	int cnt = 0;

	spin_lock(&cache->lock);
	node = cache->head;

	cache->head = NULL;
	cache->tail = NULL;

	if (cache->fname)
		kfree(cache->fname);

	/* free memory of all nodes; start with head */
	while (node) {
		tmp = node;
		node = node->next;
		kfree(tmp);
		cnt++;
	}
	printk(KERN_INFO "dnbd: freed %i cache nodes\n", cnt);

	cache->active = 0;
	spin_unlock(&cache->lock);

}

int dnbd_cache_set(dnbd_cache_t * cache, struct dnbd_file __user * arg,
		   size_t blksize)
{
	int result = -EINVAL;
	struct dnbd_file cfile;
	struct kstat stat;
	loff_t div1;
	size_t div2;

	if (cache->active) {
		printk(KERN_ERR "dnbd: cachefile is already set!\n");
		return -EFAULT;
	}

	/* open, verify and set cache file */
	if (copy_from_user(&cfile, arg, sizeof(cfile)))
		return -EFAULT;

	if (!(cache->fname = (char *) kmalloc(cfile.len + 1, GFP_KERNEL)))
		return -ENOMEM;

	if (copy_from_user
	    (cache->fname, (void __user *) cfile.name, cfile.len)) {
		result = -EFAULT;
		goto out_free;
	}
	*(cache->fname + cfile.len) = 0;

	printk(KERN_INFO "dnbd: setting cachefile to %s\n", cache->fname);

	cache->filp = filp_open(cache->fname, O_RDWR | O_LARGEFILE, 0);

	if (cache->filp == NULL || IS_ERR(cache->filp)) {
		printk(KERN_ERR "dnbd: ERROR opening cache file!\n");
		result = -EINVAL;
		goto out_free;
	}

	generic_fillattr(cache->filp->f_dentry->d_inode, &stat);

	div1 = stat.size;
	div2 = blksize;
	do_div(div1, div2);

	printk(KERN_INFO
	       "dnbd: cachefile size %llu KB using %llu blocks a %i bytes for caching.\n",
	       stat.size >> 10, div1, blksize);

	cache->max_blocks = div1;
	cache->blksize = blksize;

	/* activate cache and adapt function for insert, search and clean up */
	cache->active = 1;
	cache->insert = &dnbd_cache_insert;
	cache->search = &dnbd_cache_search;
	cache->clean = &dnbd_cache_clean;

	result = 0;
	goto out;

      out_free:
	kfree(cache->fname);
      out:
	if (result < 0 && cache->filp)
		fput(cache->filp);
	return result;
}
