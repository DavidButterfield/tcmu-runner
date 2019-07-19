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
//#include <sys/types.h>
#include <inttypes.h>
#include <errno.h>
//#include <unistd.h>
#include <sys/stat.h>

typedef struct fuse_node * fuse_node_t;

/* Application ops called by fuse_tree.c filesystem ops */
struct fuse_node_ops {
    int	    (* open)   (fuse_node_t, uintptr_t data);
    int	    (* release)(fuse_node_t, uintptr_t data);
    ssize_t (* read)   (uintptr_t data, void * buf, size_t len, loff_t ofs);
    ssize_t (* write)  (uintptr_t data, const char * buf, size_t, loff_t);
    int	    (* fsync)  (uintptr_t data, int datasync);
};

/* Return a human-readable freeable string representing the fuse tree */
extern char * fuse_tree_fmt(void);

/* Add a node with the given name under the given parent node */
extern fuse_node_t fuse_node_add(
		const char * name, fuse_node_t parent, mode_t,
		const struct fuse_node_ops *, uintptr_t data);
extern error_t fuse_node_remove(const char *name , fuse_node_t parent);

extern fuse_node_t fuse_tree_mkdir(char const * name, fuse_node_t parent);
extern error_t fuse_tree_rmdir(char const * name, fuse_node_t parent);

/* path is the full path from the fuse mount, starting with '/' */
extern fuse_node_t fuse_node_lookup(const char * path);

/* Return the private data specified to fuse_node_add */
extern uintptr_t fuse_node_data_get(fuse_node_t);

/* Update the fuse_node's mode permissions */
extern void fuse_node_update_mode(fuse_node_t, mode_t);

/* Update the fuse_node's size */
extern void fuse_node_update_size(fuse_node_t, size_t);

/* Update the fuse_node's modification time to the present */
extern void fuse_node_update_mtime(fuse_node_t);

/* Call in this order */
extern error_t fuse_tree_init(const char * mountpoint);
extern error_t fuse_loop_run(void * unused);
extern error_t fuse_tree_exit(void);

#endif /* FUSE_TREE_H */
