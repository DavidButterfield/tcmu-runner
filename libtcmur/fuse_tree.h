/* fuse_tree.h -- API to fuse filesystem tree
 *
 * Copyright 2016-2019 David A. Butterfield
 *
 * This file is licensed to you under your choice of the GNU Lesser
 * General Public License, version 2.1 or any later version (LGPLv2.1 or
 * later), or the Apache License 2.0.
 *
 * fuse_tree maintains a filesystem tree, backing fuse_operations.  Like /proc,
 * the tree itself is managed internally by the application -- there is no
 * creation of files or directories through system calls on the mounted fuse
 * filesystem.
 *
 * However, like /proc, individual files represented in the tree may be readable
 * and/or writable through the mounted filesystem, depending on permissions.
 */
#ifndef FUSE_TREE_H
#define FUSE_TREE_H
#include <sys/types.h>
#include <inttypes.h>
#include <errno.h>

/* Some names in this structure are compatible with PDE member names */
typedef struct fuse_node {
    struct fuse_node		      * parent;	    /* root's parent is NULL */
    struct fuse_node		      * sibling;    /* null terminated list */
    struct fuse_node		      * child;	    /* first child */
    const struct file_operations      *	proc_fops;  /* I/O for this fnode */
    void			      *	data;	    /* client private */
    struct inode		      * inode;
    struct module		      * owner;
    unsigned char			namelen;    /* not counting '\0' */
    char				name[1];    /* KEEP LAST */
} * fuse_node_t;

/* Return a human-readable freeable string representing the fuse tree */
extern char * fuse_tree_fmt(void);

/* Add a node with the given name under the given parent node */
struct file_operations;
extern fuse_node_t fuse_node_add(
		const char * name, fuse_node_t parent, mode_t,
		const struct file_operations *, void * data);
extern error_t fuse_node_remove(const char *name , fuse_node_t parent);

extern fuse_node_t fuse_tree_mkdir(const char * name, fuse_node_t parent);
extern error_t fuse_tree_rmdir(const char * name, fuse_node_t parent);

/* path is the full path from the fuse mount, starting with '/' */
extern fuse_node_t fuse_node_lookup(const char * path);
extern fuse_node_t fuse_node_lookupat(fuse_node_t fnode_root, const char *);

/* Return the private data specified to fuse_node_add */
extern void * fuse_node_data_get(fuse_node_t);

/* Update the fuse_node's mode permissions */
extern void fuse_node_update_mode(fuse_node_t, mode_t);

/* Update the fuse_node's size */
extern void fuse_node_update_size(fuse_node_t, size_t);

/* Update the fuse_node's block_size */
extern void fuse_node_update_block_size(fuse_node_t fnode, size_t size);

/* Update the fuse_node's modification time to the present */
extern void fuse_node_update_mtime(fuse_node_t);

/* Set the fuse_node's rdev */
extern void fuse_node_update_rdev(fuse_node_t fnode, dev_t rdev);

/* Call in this order */
extern error_t fuse_tree_init(const char * mountpoint);
extern error_t fuse_loop_run(void * unused);
extern error_t fuse_tree_exit(void);

/* fuse_bio calls XXX */
struct block_device;
extern error_t fuse_bio_init(void);
extern error_t fuse_bio_exit(void);
extern error_t fuse_bio_add(int minor, struct block_device * bdev);
extern error_t fuse_bio_remove(int minor);
extern struct file_operations fuse_bio_ops;

#endif /* FUSE_TREE_H */
