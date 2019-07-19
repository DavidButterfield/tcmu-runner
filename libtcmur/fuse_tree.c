/* fuse_tree.c -- implement a fuse filesystem tree
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
#define _GNU_SOURCE
#include <sys/types.h>
#include <inttypes.h>
#include <stdbool.h>
#include <errno.h>
#include <limits.h>
#include <stddef.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <signal.h>

#include "fuse_tree.h"
#include "sys_misc.h"

#define FUSE_USE_VERSION 26
#define _FILE_OFFSET_BITS 64	/* fuse seems to want this even on 64-bit */
#include <fuse.h>

#ifdef DEBUG
#define FLAG_NOPATH 0
#else
#define FLAG_NOPATH 1
#endif

#define trace(fmtargs...)	sys_msg(fmtargs)
#define trace_fs(fmtargs...)	// trace(fmtargs)   /* calls from fuse */
#define trace_app(fa...)	// trace(fa)	    /* calls from app */
#define trace_readdir(fa...)	// trace(fa)
#define trace_lookup(fa...)	// trace(fa)

typedef struct fuse_node {
    volatile int			refs;	    /* hold count */
    struct fuse_node		      * parent;	    /* root's parent is NULL */
    struct fuse_node		      * sibling;    /* null terminated list */
    struct fuse_node		      * child;	    /* first child */
    const struct fuse_node_ops	      *	fops;	    /* I/O for this fnode */
    uintptr_t				data;	    /* client private */
    time_t				i_atime;
    time_t				i_mtime;
    time_t				i_ctime;
    mode_t				i_mode;
    size_t				i_size;
    unsigned char			namelen;    /* not counting '\0' */
    char				name[1];    /* space for '\0' */
} * fuse_node_t;

/* Open nodes store a pointer to the fnode in fuse_file_info->fh */
#define FI_FNODE(ffi)			((fuse_node_t)(ffi)->fh)

#define foreach_child_node(parent, fnode) \
    for ((fnode) = (parent)->child; fnode; fnode = fnode->sibling)

static struct fuse_tree_ctx {
    char			      * mountpoint;
    fuse_node_t				fuse_node_root;
    sys_mutex_t				lock;
} * CTX;

#define tree_lock()		sys_mutex_lock(&CTX->lock)
#define tree_unlock()		sys_mutex_unlock(&CTX->lock)
#define assert_tree_locked()	sys_mutex_is_locked(&CTX->lock)

/*** These functions maintain the fnode tree ***/

/* fnode self-consistency check */
static void
fnode_check(struct fuse_node const * fnode)
{
    assert(fnode->refs > 0);
    assert_eq(fnode->namelen, strlen(fnode->name));
    assert(!strchr(fnode->name, '/'), "'%s'", fnode->name);
    assert_eq(!!fnode->parent, fnode != CTX->fuse_node_root);
    assert_imply(!!fnode->child, S_ISDIR(fnode->i_mode));
    assert_imply(fnode == CTX->fuse_node_root, !fnode->sibling);
    assert(S_ISREG(fnode->i_mode) || S_ISDIR(fnode->i_mode)
					 || S_ISBLK(fnode->i_mode),
	   "fnode[%s]->mode=0%2o", fnode->name, fnode->i_mode);
}

/* Return the number of direct child nodes of fnode */
static inline unsigned int
fnode_nchild(struct fuse_node const * fnode)
{
    fnode_check(fnode);
    bool isdir = S_ISDIR(fnode->i_mode);
    uint32_t count = 0;
    foreach_child_node(fnode, fnode)
	++count;
    assert_imply(!isdir, count == 0);
    return count;
}

/* Create a new fnode that can be added to the tree */
static fuse_node_t
_fnode_create(char const * name, mode_t mode,
	       const struct fuse_node_ops * fops, uintptr_t data)
{
    size_t namelen;
    assert(name);
    assert(*name);
    assert(!strchr(name, '/'), "'%s'", name);
    namelen = strlen(name);

    /* extra space for the name string -- the terminating NUL is already counted */
    fuse_node_t fnode = sys_zalloc(sizeof(*fnode) + namelen);
    if (fnode) {
	memcpy(fnode->name, name, namelen);
	assert(namelen <= UCHAR_MAX);
	fnode->namelen = (unsigned char)namelen;
	fnode->fops = fops;
	fnode->data = data;
	fnode->i_mode = mode;
	fnode->i_atime = fnode->i_mtime = fnode->i_ctime = time(NULL);
    }

    return fnode;
}

/* Free an fnode after it has been removed from the tree */
static error_t
_fnode_destroy(fuse_node_t fnode)
{
    assert(!fnode->child);	/* checked by caller */
    sys_free(fnode);
    return 0;
}

static inline void
node_hold(fuse_node_t fnode)
{
    fnode->refs++;
}

static inline void
node_drop(fuse_node_t fnode)
{
    if (!--fnode->refs)
	_fnode_destroy(fnode);
}

static inline void
assert_node_held(fuse_node_t fnode)
{
    assert_ge(fnode->refs, 2);
}

/* Add the fnode as a direct child of the parent fnode */
static fuse_node_t
fnode_add(fuse_node_t fnode, fuse_node_t parent, mode_t mode,
		   struct fuse_node_ops	const * fops, uintptr_t data)
{
    assert(parent);
    assert(S_ISDIR(parent->i_mode));
    assert_tree_locked();

    node_hold(fnode);

    fnode->i_mode = mode;
    fnode->parent = parent;
    fnode->sibling = fnode->parent->child;
    parent->child = fnode;
    parent->i_size++;
    fuse_node_update_mtime(parent);

    trace_app("created %sfnode %s under %s",
		S_ISDIR(mode)?"DIRECTORY ":"", fnode->name, parent->name);
    fnode_check(fnode);
    return fnode;
}

/* Remove the fnode as a direct child of the parent fnode */
static error_t
fnode_remove(fuse_node_t fnode, fuse_node_t parent)
{
    trace_app("%s/%s", parent->name, fnode->name);
    assert_tree_locked();
    assert(S_ISDIR(parent->i_mode));
    assert_eq(fnode->child, NULL);
    assert_eq(fnode->parent, parent);

    if (fnode->refs > 1)
	return -EBUSY;			/* fnode is open by someone */

    fuse_node_t * nodep;
    for (nodep = &parent->child; *nodep; nodep = &(*nodep)->sibling) {
	fnode_check(*nodep);
	if (*nodep != fnode)
	    continue;

	*nodep = (*nodep)->sibling;	/* remove from list */
	parent->i_size--;
	fuse_node_update_mtime(parent);

	node_drop(fnode);
	return 0;
    }

    assert(false, "Failed to find child %s under parent %s!",
		fnode->name, parent->name);
    return -ENOENT;	    /* fnode not found */
}

/******************************************************************************/
static fuse_node_t fnode_lookup(fuse_node_t fnode_root, char const * path);

/* These are called by the application program to build and operate on an fnode
 * tree rooted at CTX->fuse_node_root.
 */

/* Add an entry to the tree directly under parent --
 * attaches to CTX->fuse_node_root if parent is NULL
 */
fuse_node_t
fuse_node_add(const char * name, fuse_node_t parent, mode_t mode,
		const struct fuse_node_ops * fops, uintptr_t data)
{
    if (!parent)
	parent = CTX->fuse_node_root;

    if (!(mode & S_IFMT))
	mode |= S_IFREG;

    trace_app("%s/%s", parent->name, name);
    tree_lock();

    if (fnode_lookup(parent, name)) {
	sys_warning("attempt to create %s/%s which already exists",
		    parent->name, name);
	tree_unlock();
	return NULL;
    }

    fuse_node_t fnode = _fnode_create(name, mode, fops, data);
    if (fnode)
	fnode_add(fnode, parent, mode, fops, data);
    else
	sys_warning("failed to create %sfnode %s under %s",
		    S_ISDIR(mode)?"DIRECTORY ":"", name, parent->name);

    tree_unlock();
    return fnode;
}

/* Remove an entry from directly under parent */
error_t
fuse_node_remove(char const * name, fuse_node_t parent)
{
    error_t err;

    if (!parent)
	parent = CTX->fuse_node_root;

    trace_app("%s/%s", parent->name, name);
    tree_lock();

    fuse_node_t fnode = fnode_lookup(parent, name);

    if (fnode->child) {
	sys_warning("fnode[%s] still has %d child(ren) e.g. '%s'",
	    fnode->name, fnode_nchild(fnode), fnode->child->name);
	return -ENOTEMPTY;	/* can't remove a DIR with children */
    }

    err = fnode_remove(fnode, parent);

    tree_unlock();
    return err;
}

fuse_node_t
fuse_tree_mkdir(char const * name, fuse_node_t parent)
{
    return fuse_node_add(name, parent, S_IFDIR|0555, NULL, 0);
}

error_t
fuse_tree_rmdir(char const * name, fuse_node_t parent)
{
    error_t err = fuse_node_remove(name, parent);
    if (err)
	fprintf(stderr, "fuse_tree_rmdir %s return %d\n", name, err);
    return err;
}

/* Update the fuse_node's mode permissions */
void
fuse_node_update_mode(fuse_node_t fnode, mode_t mode)
{
    fnode->i_mode = (mode_t)((fnode->i_mode & ~0777u) | (mode & 0777u));
}

/* Update the fuse_node's size */
void
fuse_node_update_size(fuse_node_t fnode, size_t size)
{
    fnode->i_size = size;
}

/* Update the fuse_node's modification time to the present */
void
fuse_node_update_mtime(fuse_node_t fnode)
{
    fnode->i_mtime = time(NULL);
}

/******************************************************************************/
/*** These functions operate on one particular fnode in the tree ***/

uintptr_t
fuse_node_data_get(fuse_node_t fnode)
{
    return fnode->data;
}

/* Pass back the attributes of the specified fnode */
static error_t
fnode_getattr(fuse_node_t fnode, struct stat * st)
{
    st->st_mode = fnode->i_mode;

    /* Trouble is that if we make the fnode appear as a block device to users
     * of the fuse mount, fuse additionally assumes that to mean to let the
     * kernel interpret the dev_t as referring to a kernel major/minor, instead
     * of letting our handlers interpret them.
     */
    if (S_ISBLK(fnode->i_mode))
	st->st_mode = S_IFREG | (fnode->i_mode & 0777);

    st->st_nlink = 1u + fnode_nchild(fnode);   /* assume no . or .. */
    st->st_uid = geteuid();
    st->st_gid = getegid();
    st->st_size = (ssize_t)fnode->i_size;
    st->st_atime = fnode->i_atime;
    st->st_mtime = fnode->i_mtime;
    st->st_ctime = fnode->i_ctime;
    // st->st_rdev = fnode->i_rdev;
    // st->st_blocks		    // blocks allocated

    return 0;
}

/* Pass back a list of children of a directory fnode, starting at child index ofs */
static error_t
fnode_readdir(fuse_node_t fnode, void * buf, fuse_fill_dir_t filler, off_t ofs)
{
    assert(S_ISDIR(fnode->i_mode));
    fnode->i_atime = time(NULL);
    off_t next_idx = ofs;
    foreach_child_node(fnode, fnode) {
	fnode_check(fnode);
	if (ofs) {
	    --ofs;
	    continue;	/* skip over the first (ofs) items without processing */
	}
	trace_readdir("    %s child=%s", path, fnode->name);
	if (filler(buf, fnode->name, NULL, ++next_idx))
	    break;	/* buffer full */
    }
    return 0;
}

/* Call the fnode's open function */
static error_t
fnode_open(struct fuse_file_info * fi)
{
    error_t err = 0;
    fuse_node_t fnode = FI_FNODE(fi);
    assert(fnode);

    if (fnode->fops && fnode->fops->open)
	err = fnode->fops->open(fnode, fnode->data);

    return err;
}

/* Call the fnode's release function */
static error_t
fnode_release(struct fuse_file_info * fi)
{
    error_t err = 0;
    fuse_node_t fnode = FI_FNODE(fi);
    assert(fnode);

    if (fnode->fops && fnode->fops->release)
	err = fnode->fops->release(fnode, fnode->data);

    return err;
}

/* Read into buf up to size bytes starting at ofs.
 * Returns bytes read, or -errno.
 */
static ssize_t
fnode_read(struct fuse_file_info * fi, char * buf, size_t size, off_t ofs)
{
    fuse_node_t fnode = FI_FNODE(fi);
    assert(fnode);
    if (!fnode->fops || !fnode->fops->read)
	return -EINVAL;

    ssize_t bytes_read = fnode->fops->read(fnode->data, buf, size, ofs);
    if (bytes_read < 0)
	sys_warning("fnode[%s]->fops->read got %ld", fnode->name, bytes_read);
    else
	fnode->i_atime = time(NULL);

    return bytes_read;
}

/* Write size bytes from buf starting at ofs.
 * Returns bytes written, or -errno.
 */
static ssize_t
fnode_write(struct fuse_file_info * fi, char const * buf, size_t size, off_t ofs)
{
    fuse_node_t fnode = FI_FNODE(fi);
    assert(fnode);
    if (!fnode->fops || !fnode->fops->write)
	return -EINVAL;

    ssize_t bytes_written = fnode->fops->write(fnode->data, buf, size, ofs);
    if (bytes_written != (ssize_t)size)
	sys_warning("fnode[%s]->fops->write got %ld",
			fnode->name, bytes_written);
    else
	fnode->i_mtime = time(NULL);

    return bytes_written;
}

/* fsync the fnode.  Returns zero for success, or -errno */
static error_t
fnode_fsync(struct fuse_file_info * fi, int datasync)
{
    fuse_node_t fnode = FI_FNODE(fi);
    assert(fnode);
    if (!fnode->fops || !fnode->fops->fsync)
	return 0;	    /* right? */

    error_t err = fnode->fops->fsync(fnode->data, datasync);
    if (err)
	sys_warning("fnode[%s]->fops->fsync got %d", fnode->name, err);
    else
	fnode->i_mtime = time(NULL);

    return err;
}

/******************************************************************************/
/* These operate on a subtree starting at fnode_root, with path names relative thereto
 * (fnode_root can be any position in the tree, even a leaf)
 */

/* Try to find an fnode matching path name, starting at the given fnode_root --
 * Returns NULL if no matching fnode is found.
 */
static fuse_node_t
fnode_lookup(fuse_node_t fnode_root, char const * path)
{
    fuse_node_t fnode;
    trace_lookup("%s", path);
    fnode_check(fnode_root);
    assert(S_ISDIR(fnode_root->i_mode));
    assert_tree_locked();

    while (*path == '/')
	path++;
    if (*path == '\0') {
	fnode_check(fnode_root);
	return fnode_root;    /* path string ended at this fnode */
    }

    uint32_t name_ofs;	    /* offset into fnode's name string */
    foreach_child_node(fnode_root, fnode) {
	fnode_check(fnode);
	for (name_ofs = 0 ; path[name_ofs] == fnode->name[name_ofs]; name_ofs++)
	    if (fnode->name[name_ofs] == '\0')
		break;	/* end of matching strings */

	if (fnode->name[name_ofs] != '\0')
	    continue;	/* mismatch -- try the next sibling */

	if (path[name_ofs] != '\0' && path[name_ofs] != '/')
	    continue;	/* mismatch -- fnode name was shorter */

	/* Found an entry matching this path segment */
	if (path[name_ofs] == '\0') {
	    fnode_check(fnode);
	    return fnode;		    /* this was the last path segment */
	}

	/* Descend (recursion) to lookup the next path segment with fnode as root */
	return fnode_lookup(fnode, path + name_ofs);
    }

    return NULL;	/* not found */
}

//XXX this should "hold" the looked-up fnode, and add a drop function
fuse_node_t
fuse_node_lookup(const char * path)
{
    fuse_node_t fnode;
    tree_lock();
    fnode = fnode_lookup(CTX->fuse_node_root, path);
    tree_unlock();
    return fnode;
}

/* Format a subtree into a debugging string representation */
static char *
_tree_fmt(fuse_node_t fnode_root, uint32_t level)
{
    fuse_node_t fnode = fnode_root;
    char * str;
    int ret = sys_asprintf(&str,
	    "%*snode@%p={name='%s' mode=0%o%s size=%ld refs=%d}\n", level*4, "",
	    fnode, fnode->name, fnode->i_mode,
		S_ISDIR(fnode->i_mode) ? " (DIR)" :
		S_ISBLK(fnode->i_mode) ? " (BLK)" :
		S_ISREG(fnode->i_mode) ? " (REG)" : "",
	    fnode->i_size, fnode->refs);
    if (ret < 0)
	return NULL;

    foreach_child_node(fnode, fnode)
	str = sys_string_concat_free(str, _tree_fmt(fnode, level + 1));

    return str;
}

char *
fuse_tree_fmt(void)
{
    char * str;
    fuse_node_t fnode_root = CTX->fuse_node_root;
    if (!fnode_root)
	return sys_zalloc(1);	/* empty string */

    tree_lock();
    str = _tree_fmt(fnode_root, 0);
    tree_unlock();
    return str;
}

/* Callable under gdb to dump out the fuse tree */
static void __attribute__((__unused__))
tree_dump(void)
{
    char * str;
    fuse_node_t fnode_root = CTX->fuse_node_root;
    if (fnode_root) {
	str = _tree_fmt(fnode_root, 0);
	fprintf(stderr, "%s", str);
	sys_free(str);
    }
}

/******************************************************************************/
/* These are called by FUSE to implement the filesystem functions */

static error_t
op_fuse_getattr(char const * path, struct stat * st)
{
    error_t err = -ENOENT;
    // trace_fs("%s", path);
    tree_lock();

    fuse_node_t fnode = fnode_lookup(CTX->fuse_node_root, path);
    if (fnode)
	err = fnode_getattr(fnode, st);

    tree_unlock();
    return err;
}

static error_t
op_fuse_readdir(char const * path, void * buf,
		 fuse_fill_dir_t filler, off_t ofs, struct fuse_file_info * fi)
{
    error_t err = -ENOENT;
    // trace_fs("%s ofs=%"PRIu64, path, ofs);
    tree_lock();

    fuse_node_t fnode = fnode_lookup(CTX->fuse_node_root, path);
    if (fnode) {
	if (!S_ISDIR(fnode->i_mode))
	    err = -ENOTDIR;
	else
	    err = fnode_readdir(fnode, buf, filler, ofs);
    }

    tree_unlock();
    return err;
}

static error_t
op_fuse_open(char const * path, struct fuse_file_info * fi)
{
    error_t err = -ENOENT;
    trace_fs("%s", path);

    tree_lock();

    fuse_node_t fnode = fnode_lookup(CTX->fuse_node_root, path);
    if (fnode) {
	if (S_ISDIR(fnode->i_mode)) {
	    tree_unlock();
	    err = -EISDIR;
	} else {
	    node_hold(fnode);
	    tree_unlock();
	    fi->fh = (uintptr_t)fnode;	/* stash fnode pointer in fuse info */
	    err = fnode_open(fi);
	    if (err) {
		sys_warning("fnode[%s]->fops->open returned %d",
				fnode->name, err);
		fi->fh = (uintptr_t)NULL;
		node_drop(fnode);
	    }
	}
    } else {
	tree_unlock();
    }

    if (!err && !S_ISBLK(fnode->i_mode)) {
	fi->nonseekable = true;
	fi->direct_io = true;
    }

    return err;
}

static error_t
op_fuse_release(char const * path, struct fuse_file_info * fi)
{
    error_t err = -EINVAL;
    trace_fs("%s", path);

    fuse_node_t fnode = FI_FNODE(fi);

    if (!FLAG_NOPATH)
	assert_eq(fnode, ({ fuse_node_t foo = fuse_node_lookup(path); foo; }));

    if (fnode) {
	if (!S_ISDIR(fnode->i_mode)) {
	    err = fnode_release(fi);
	    if (err)
		sys_warning("fnode[%s]->fops->release got %d",
				    fnode->name, err);
	    else {
		node_drop(fnode);
	    }
	}
    }

    if (!err)
	fi->fh = (uintptr_t)NULL;

    return err;
}

/* For op_fuse_read, _write, and _fsync the fnode was held by _open */
static int /*ssize_t?*/
op_fuse_read(char const * path, char * buf, size_t size, off_t ofs, struct fuse_file_info * fi)
{
    ssize_t ret = -EBADF;
    buf[0] = '\0';
    trace_fs("%s size=%lu ofs=%"PRIu64, path, size, ofs);

    fuse_node_t fnode = FI_FNODE(fi);

    if (!FLAG_NOPATH)
	assert_eq(fnode, ({ fuse_node_t foo = fuse_node_lookup(path); foo; }));

    if (fnode) {
	assert_node_held(fnode);
	if (S_ISDIR(fnode->i_mode))
	    ret = -EISDIR;
	else
	    ret = fnode_read(fi, buf, size, ofs);

	if (ret >= 0)
	    fnode->i_atime = time(NULL);
    }

    trace_fs("READ %s REPLY len=%"PRIu64, path, ret);
    return (int)ret;
}

static int /*ssize_t?*/
op_fuse_write(char const * path, char const * buf, size_t size, off_t ofs, struct fuse_file_info * fi)
{
    ssize_t ret = -EBADF;
    trace_fs("%s size=%lu ofs=%"PRIu64, path, size, ofs);

    fuse_node_t fnode = FI_FNODE(fi);

    if (!FLAG_NOPATH)
	assert_eq(fnode, ({ fuse_node_t foo = fuse_node_lookup(path); foo; }));

    if (fnode) {
	assert_node_held(fnode);
	if (S_ISDIR(fnode->i_mode))
	    ret = -EISDIR;
	else
	    ret = fnode_write(fi, buf, size, ofs);

	if (ret >= 0)
	    fnode->i_mtime = time(NULL);
    }

    trace_fs("WRITE %s REPLY len=%"PRIu64, path, ret);
    return (int)ret;
}

static error_t
op_fuse_fsync(char const * path, int datasync, struct fuse_file_info * fi)
{
    error_t err = -EBADF;
    trace_fs("FSYNC %s %d", path, datasync);

    fuse_node_t fnode = FI_FNODE(fi);

    if (!FLAG_NOPATH)
	assert_eq(fnode, ({ fuse_node_t foo = fuse_node_lookup(path); foo; }));

    if (fnode) {
	assert_node_held(fnode);
	if (S_ISDIR(fnode->i_mode))
	    err = -EISDIR;
	else
	    err = fnode_fsync(fi, datasync);
    }

    trace_fs("FSYNC %s REPLY err=%d", path, err);
    return err;
}

static struct fuse_operations const fnode_ops = {
    .getattr	= op_fuse_getattr,
    .open	= op_fuse_open,
    .release	= op_fuse_release,
    .read	= op_fuse_read,
    .write	= op_fuse_write,
    .fsync	= op_fuse_fsync,
    .readdir	= op_fuse_readdir,
    .flag_nopath = FLAG_NOPATH,
};

/******************************************************************************/

/* mountpoint must start with '/' */
error_t
fuse_tree_init(char const * mountpoint)
{
    assert(mountpoint);

    if (*mountpoint != '/')
	return -EINVAL;

    char const * rootname = 1 + strrchr(mountpoint, '/');
    if (*rootname == '\0')
	return -EINVAL;

    assert(!CTX);
    CTX = sys_zalloc(sizeof(*CTX));
    CTX->mountpoint = sys_strdup(mountpoint);

    /* Create the root fnode that overlays the mount point */
    CTX->fuse_node_root = _fnode_create(rootname, S_IFDIR|0555, NULL, 0);
    node_hold(CTX->fuse_node_root);

    fnode_check(CTX->fuse_node_root);
    return 0;
}

error_t
fuse_tree_exit(void)
{
    fuse_node_t root = CTX->fuse_node_root;
    assert(root);
    assert(CTX);
    assert(CTX->mountpoint);

    if (fnode_nchild(root)) {
	sys_warning("fuse root fnode[%s] still has %d child(ren) e.g. '%s'",
	    root->name, fnode_nchild(root), root->child->name);
	return -EBUSY;
    }

    _fnode_destroy(root);
    sys_free(CTX->mountpoint);
    sys_free(CTX);
    CTX = NULL;

    return 0;
}

/* Run the fuse loop */
//XXX pass in a threading option
error_t
fuse_loop_run(void * unused)
{
    error_t err;
    assert_eq(unused, NULL);
    assert(CTX);

    /* Create the mount point for the fuse filesystem */
    {
	char * cmd;
	int ret = sys_asprintf(&cmd, "/bin/mkdir -p %s; chmod 777 %s",
				    CTX->mountpoint, CTX->mountpoint);
	if (ret < 0)
	    return -errno;

	int rc = system(cmd);
	if (rc)
	    sys_warning("'%s' returns %d", cmd, rc);
	else
	    sys_notice("created fuse root %s", CTX->mountpoint);
	sys_free(cmd);
    }

    char /* const */ * argv[] = {
	"fuse_main",		    /* argv[0] */
	CTX->mountpoint,
	//"--help",
	//"--version",
	//"-d",			    /* debug, implies -f */

	"-f",			    /* foreground (else daemonizes) */
	"-s",			    /* single-threaded */
	"-o", "subtype=fnode",	    /* third field in /etc/mtab */
	"-o", "allow_other",	    /* any user can access our fuse tree */
	"-o", "auto_unmount",	    /* unmount fuse fs when program exits */

	// "-o", "auto_cache",	    /* invalidate kernel cache on each open */

	// "-o", "sync_read",	    /* perform all reads synchronously */
	// "-o", "sync",	    /* perform all I/O synchronously */
	// "-o", "max_readahead=0", /* max bytes to read-ahead */

	"-o", "atomic_o_trunc",	    /* avoid calls to truncate */
	"-o", "default_permissions",/* fuse do mode permission checking */
	// "-o", "dev",		    /* allow device nodes */
				    // XXX interpreted as KERNEL dev_t!

	NULL			    /* end of argv[] list */
    };

    int argc = sizeof(argv)/sizeof(argv[0]) - 1/*NULL*/;

    trace("Calling fuse loop on tid=%u", gettid());
    err = fuse_main(argc, argv, &fnode_ops, NULL);
    trace("Returned err=%d from fuse loop on tid=%u", err, gettid());
    return err;
}
