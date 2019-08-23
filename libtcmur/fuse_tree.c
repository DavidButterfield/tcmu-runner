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
#include <signal.h>

#include "sys_impl.h"
#include "fuse_tree.h"
#include "sys_assert.h" /* include after tcmu-runner.h (libtcmur.h) */

#include <string.h>	    /* include  after sys_impl.h */

#define FUSE_USE_VERSION 26
#define _FILE_OFFSET_BITS 64	/* fuse seems to want this even on 64-bit */
#include <fuse.h>

#ifdef DEBUG
#define FLAG_NOPATH 0
#else
#define FLAG_NOPATH 1
#endif

#define trace_app(fa...)	//  nlprintk(fa)	/* from app */
#define trace_fs(fmtargs...)	//  nlprintk(fmtargs)	/* from fuse */
#define trace_readdir(fa...)	//  nlprintk(fa)	/* from fuse */
#define trace_getattr(fa...)	//  nlprintk(fa)	/* from fuse */
#define trace_lookup(fa...)	//  nlprintk(fa)

/* Open nodes store a pointer to the fnode in fuse_file_info->fh */
#define FI_FILE(fi)			((struct file *)(fi)->fh)
#define FI_FNODE(fi)			(FI_FILE(fi)->inode->pde)

#define foreach_child_node(parent, fnode) \
    for ((fnode) = (parent)->child; fnode; fnode = fnode->sibling)

static struct fuse_tree_ctx {
    char			      * mountpoint;
    fuse_node_t				fuse_node_root;
    struct mutex			lock;
} * CTX;

#define tree_lock()		do { assert(CTX); \
				     mutex_lock(&CTX->lock); } while (0)
#define tree_unlock()		mutex_unlock(&CTX->lock)
#define assert_tree_locked()	assert(mutex_is_locked(&CTX->lock))

/*** These functions maintain the fnode tree ***/

/* fnode self-consistency check */
static void
fnode_check(struct fuse_node const * fnode)
{
    assert_ge(atomic_get(&fnode->inode->i_count), 1);
    assert_eq(fnode->namelen, strlen(fnode->name));
    assert_eq(strchr(fnode->name, '/'), NULL, "'%s'", fnode->name);
    assert_eq(!!fnode->parent, fnode != CTX->fuse_node_root);
    assert_imply(!!fnode->child, S_ISDIR(fnode->inode->i_mode));
    assert_imply(fnode == CTX->fuse_node_root, !fnode->sibling);
    assert(S_ISREG(fnode->inode->i_mode) || S_ISDIR(fnode->inode->i_mode)
					 || S_ISBLK(fnode->inode->i_mode),
	   "fnode[%s]->mode=0%2o", fnode->name, fnode->inode->i_mode);
}

/* Return the number of direct child nodes of fnode */
static inline unsigned int
fnode_nchild(struct fuse_node const * fnode)
{
    fnode_check(fnode);
    bool isdir = S_ISDIR(fnode->inode->i_mode);
    unsigned int count = 0;
    foreach_child_node(fnode, fnode)
	++count;
    assert_imply(!isdir, count == 0);
    return count;
}

static void
_fnode_destructor(struct inode * inode)
{
    vfree(inode->pde);
    vfree(inode);
}

/* Create a new fnode that can be added to the tree */
static fuse_node_t
_fnode_create(const char * name, mode_t mode,
	       const struct file_operations * fops, void * data)
{
    size_t namelen;
    assert(name);
    assert_ne(*name, '\0');
    assert_eq(strchr(name, '/'), NULL, "'%s'", name);
    namelen = strlen(name);

    /* extra space for the name string -- the terminating NUL is already counted */
    fuse_node_t fnode = vzalloc(sizeof(*fnode) + namelen);
    if (fnode) {
	memcpy(fnode->name, name, namelen);
	assert_le(namelen, UCHAR_MAX);
	fnode->namelen = (unsigned char)namelen;
	fnode->proc_fops = fops;
	fnode->data = data;
	fnode->inode = vzalloc(sizeof(*fnode->inode));
	init_inode(fnode->inode, I_TYPE_PROC, mode, 0, 0, -1);
	fnode->inode->pde = fnode;
	fnode->inode->UMC_destructor = _fnode_destructor;
    }

    return fnode;
}

static inline void
node_hold(fuse_node_t fnode)
{
    _iget(fnode->inode);
}

static inline void
node_drop(fuse_node_t fnode)
{
    iput(fnode->inode);
}

static inline void
assert_node_held(fuse_node_t fnode)
{
    assert_ge(atomic_get(&fnode->inode->i_count), 2);
}

/* Add the fnode as a direct child of the parent fnode */
static fuse_node_t
fnode_add(fuse_node_t fnode, fuse_node_t parent)
{
    assert(parent);
    assert(S_ISDIR(parent->inode->i_mode));
    assert_tree_locked();

    fnode->parent = parent;
    fnode->sibling = parent->child;
    parent->child = fnode;
    parent->inode->i_size++;
    fuse_node_update_mtime(parent);

    trace_app("created %sfnode %s under %s",
	    S_ISDIR(fnode->inode->i_mode)?"DIRECTORY ":"", fnode->name, parent->name);
    fnode_check(fnode);
    return fnode;
}

/* Remove the fnode as a direct child of the parent fnode */
static error_t
fnode_remove(fuse_node_t fnode, fuse_node_t parent)
{
    trace_app("%s/%s", parent->name, fnode->name);
    assert_tree_locked();
    assert(S_ISDIR(parent->inode->i_mode));
    assert_eq(fnode->child, NULL);
    assert_eq(fnode->parent, parent);

    if (atomic_get(&fnode->inode->i_count) > 1)
	return -EBUSY;			/* fnode is open by someone */

    fuse_node_t * nodep;
    for (nodep = &parent->child; *nodep; nodep = &(*nodep)->sibling) {
	fnode_check(*nodep);
	if (*nodep != fnode)
	    continue;

	*nodep = (*nodep)->sibling;	/* remove from list */
	parent->inode->i_size--;
	fuse_node_update_mtime(parent);

	trace_app("removed %sfnode %s under %s",
		S_ISDIR(fnode->inode->i_mode)?"DIRECTORY ":"", fnode->name, parent->name);

	node_drop(fnode);
	return 0;
    }

    pr_warning("Failed to find child %s under parent %s!\n",
		fnode->name, parent->name);
    return -ENOENT;	    /* fnode not found */
}

/******************************************************************************/
static fuse_node_t fnode_lookup(fuse_node_t fnode_root, const char * path);

/* These are called by the application program to build and operate on an fnode
 * tree rooted at CTX->fuse_node_root.
 */

/* Add an entry to the tree directly under parent --
 * attaches to CTX->fuse_node_root if parent is NULL
 */
fuse_node_t
fuse_node_add(const char * name, fuse_node_t parent, mode_t mode,
		const struct file_operations * fops, void * data)
{
    verify(CTX, "fuse_node_add called before fuse_tree_init");

    if (!parent)
	parent = CTX->fuse_node_root;

    if (!(mode & S_IFMT))
	mode |= S_IFREG;

    trace_app("%s/%s", parent->name, name);
    tree_lock();

    fuse_node_t fnode = fnode_lookup(parent, name);
    if (fnode) {
	/* Node already exists: if directory, just return it */
	if (!S_ISDIR(mode) || !S_ISDIR(fnode->inode->i_mode)) {
	    /* Not a directory, fail */
	    pr_warning("attempt to create %s/%s which already exists\n",
			parent->name, name);
	    fnode = NULL;
	}
	tree_unlock();
	return fnode;
    }

    fnode = _fnode_create(name, mode, fops, data);
    if (fnode) {
	if (S_ISBLK(mode))
	    fnode->inode->i_blkbits = 9;    /* default 512-byte blocks */
	fnode_add(fnode, parent);
    } else
	pr_warning("failed to create %sfnode %s under %s\n",
		    S_ISDIR(mode)?"DIRECTORY ":"", name, parent->name);

    tree_unlock();
    return fnode;
}

/* Remove an entry from directly under parent */
error_t
fuse_node_remove(const char * name, fuse_node_t parent)
{
    error_t err;

    if (!parent)
	parent = CTX->fuse_node_root;

    trace_app("%s/%s", parent->name, name);
    tree_lock();

    fuse_node_t fnode = fnode_lookup(parent, name);

    if (fnode->child) {
	pr_warning("fnode[%s] still has %d child(ren) e.g. '%s'\n",
	    fnode->name, fnode_nchild(fnode), fnode->child->name);
	err = -ENOTEMPTY;	/* can't remove a DIR with children */
    } else
	err = fnode_remove(fnode, parent);

    tree_unlock();
    return err;
}

fuse_node_t
fuse_tree_mkdir(const char * name, fuse_node_t parent)
{
    return fuse_node_add(name, parent, S_IFDIR|0555, NULL, 0);
}

error_t
fuse_tree_rmdir(const char * name, fuse_node_t parent)
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
    trace_app("%s", fnode->name);
    fnode->inode->i_mode = (mode_t)((fnode->inode->i_mode & ~0777u) | (mode & 0777u));
}

/* Update the fuse_node's size */
void
fuse_node_update_size(fuse_node_t fnode, size_t size)
{
    trace_app("%s size %ld-->%ld", fnode->name, fnode->inode->i_size, size);
    fnode->inode->i_size = (off_t)size;
}

#ifndef ilog2
#define ilog2(v)    ((v) ? 63 - __builtin_clzl((uint64_t)(v)) : -1)
#endif

/* Update the fuse_node's block size */
void
fuse_node_update_block_size(fuse_node_t fnode, size_t size)
{
    int blkbits = ilog2(size);
    assert(size);
    assert_ge(blkbits, 0);
    assert_eq(size, 1ul << blkbits, "not a power of two");
    trace_app("%s i_blkbits %d-->%d", fnode->name, fnode->inode->i_blkbits, blkbits);
    fnode->inode->i_blkbits = (unsigned int)blkbits;
}

/* Update the fuse_node's modification time to the present */
void
fuse_node_update_mtime(fuse_node_t fnode)
{
    fnode->inode->i_mtime = time(NULL);
    trace_app("%s", fnode->name);
}

/* Set the fuse_node's rdev */
void
fuse_node_update_rdev(fuse_node_t fnode, dev_t rdev)
{
    fnode->inode->i_rdev = rdev;
    trace_app("%s", fnode->name);
}

/******************************************************************************/
/*** These functions operate on one particular fnode in the tree ***/

void *
fuse_node_data_get(fuse_node_t fnode)
{
    return fnode->data;
}

/* Pass back the attributes of the specified fnode */
static error_t
fnode_getattr(fuse_node_t fnode, struct stat * st)
{
    st->st_mode = fnode->inode->i_mode;
#if 1	//XXX make ISBLK appear as IFREG through fuse
    /* Trouble is that if we make the fnode appear as a block device to users
     * of the fuse mount, fuse additionally assumes that to mean to let the
     * kernel interpret the dev_t as referring to a kernel major/minor, instead
     * of letting our handlers interpret them.
     */
    if (S_ISBLK(fnode->inode->i_mode))
	st->st_mode = S_IFREG | (fnode->inode->i_mode & 0777);
#endif

    st->st_nlink = 1u + fnode_nchild(fnode);   /* assume no . or .. */
    st->st_uid = geteuid();
    st->st_gid = getegid();
    st->st_size = (ssize_t)fnode->inode->i_size;
    st->st_atime = fnode->inode->i_atime;
    st->st_mtime = fnode->inode->i_mtime;
    st->st_ctime = fnode->inode->i_ctime;
    st->st_rdev = fnode->inode->i_rdev;
    st->st_blksize = 1L << fnode->inode->i_blkbits;
    return 0;
}


/* Pass back a list of children of a directory fnode, starting at child index ofs */
static error_t
fnode_readdir(fuse_node_t fnode, void * buf, fuse_fill_dir_t filler, off_t ofs)
{
    assert(S_ISDIR(fnode->inode->i_mode));
    fnode->inode->i_atime = time(NULL);
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

    if (fnode->proc_fops && fnode->proc_fops->open)
	err = fnode->proc_fops->open(file_inode(FI_FILE(fi)), FI_FILE(fi));

    return err;
}

/* Call the fnode's release function */
static error_t
fnode_release(struct fuse_file_info * fi)
{
    error_t err = 0;
    fuse_node_t fnode = FI_FNODE(fi);
    assert(fnode);

    if (fnode->proc_fops && fnode->proc_fops->release)
	err = fnode->proc_fops->release(file_inode(FI_FILE(fi)), FI_FILE(fi));

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
    if (!fnode->proc_fops || !fnode->proc_fops->read)
	return -EINVAL;

    ssize_t bytes_read = fnode->proc_fops->read(FI_FILE(fi), buf, size, &ofs);
    if (bytes_read < 0)
	pr_warning("fnode[%s]->proc_fops->read(bytes=%ld @ ofs=%ld got %ld\n",
		    fnode->name, size, ofs, bytes_read);
    else
	fnode->inode->i_atime = time(NULL);

    return bytes_read;
}

/* Write size bytes from buf starting at ofs.
 * Returns bytes written, or -errno.
 */
static ssize_t
fnode_write(struct fuse_file_info * fi, const char * buf, size_t size, off_t ofs)
{
    fuse_node_t fnode = FI_FNODE(fi);
    assert(fnode);
    if (!fnode->proc_fops || !fnode->proc_fops->write)
	return -EINVAL;

    ssize_t bytes_written = fnode->proc_fops->write(FI_FILE(fi), buf, size, &ofs);
    if (bytes_written != (ssize_t)size)
	pr_warning("fnode[%s]->proc_fops->write(bytes=%ld @ ofs=%ld got %ld\n",
		    fnode->name, size, ofs, bytes_written);
    else
	fnode->inode->i_mtime = time(NULL);

    return bytes_written;
}

/* fsync the fnode.  Returns zero for success, or -errno */
static error_t
fnode_fsync(struct fuse_file_info * fi, int datasync)
{
    fuse_node_t fnode = FI_FNODE(fi);
    assert(fnode);
    if (!fnode->proc_fops || !fnode->proc_fops->fsync)
	return 0;	    /* right? */

    error_t err = fnode->proc_fops->fsync(FI_FILE(fi), datasync);
    if (!err)
	fnode->inode->i_mtime = time(NULL);
    WARN_ONCE(err, "fnode[%s]->proc_fops->fsync got %d\n", fnode->name, err);

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
fnode_lookup(fuse_node_t fnode_root, const char * path)
{
    fuse_node_t fnode;
    trace_lookup("%s", path);
    fnode_check(fnode_root);
    assert(S_ISDIR(fnode_root->inode->i_mode),"%s has mode 0%02o",
		    fnode_root->name, fnode_root->inode->i_mode);
    assert_tree_locked();

    while (*path == '/')
	path++;
    if (*path == '\0') {
	fnode_check(fnode_root);
	return fnode_root;    /* path string ended at this fnode */
    }

    off_t name_ofs;	    /* offset into fnode's name string */
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
fuse_node_lookupat(fuse_node_t fnode_root, const char * path)
{
    fuse_node_t fnode;
    tree_lock();
    fnode = fnode_lookup(fnode_root, path);
    tree_unlock();
    return fnode;
}

fuse_node_t
fuse_node_lookup(const char * path)
{
    verify(CTX, "fuse_node_lookup called before fuse_tree_init");
    return fuse_node_lookupat(CTX->fuse_node_root, path);
}

/* Format a subtree into a debugging string representation */
static char *
_tree_fmt(fuse_node_t fnode_root, int level)
{
    char * str;
    fuse_node_t fnode = fnode_root;
    if (!fnode)
	return NULL;

    str = kasprintf(0,
	    "%*snode@%p={name='%s' parent@%p mode=0%o%s size=%ld refs=%d}\n", level*4, "",
	    fnode, fnode->name, fnode->parent, fnode->inode->i_mode,
		S_ISDIR(fnode->inode->i_mode) ? " (DIR)" :
		S_ISBLK(fnode->inode->i_mode) ? " (BLK)" :
		S_ISREG(fnode->inode->i_mode) ? " (REG)" : "",
	    fnode->inode->i_size, atomic_get(&fnode->inode->i_count));

    foreach_child_node(fnode, fnode)
	str = UMC_string_concat_free(str, _tree_fmt(fnode, level + 1));

    return str;
}

char *
fuse_tree_fmt(void)
{
    char * str;
    fuse_node_t fnode_root;
    if (!CTX)
	return NULL;

    fnode_root = CTX->fuse_node_root;
    if (!fnode_root)
	return NULL;

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
    fuse_node_t fnode_root;
    if (CTX) {
	fnode_root = CTX->fuse_node_root;
	if (fnode_root) {
	    str = _tree_fmt(fnode_root, 0);
	    fprintf(stderr, "%s", str);
	    vfree(str);
	}
    }
}

/******************************************************************************/
/* These are called by FUSE to implement the filesystem functions */

static error_t
op_fuse_getattr(const char * path, struct stat * st)
{
    error_t err = -ENOENT;
    trace_getattr("%s", path);
    tree_lock();

    fuse_node_t fnode = fnode_lookup(CTX->fuse_node_root, path);
    if (fnode)
	err = fnode_getattr(fnode, st);

    tree_unlock();
    return err;
}

static error_t
op_fuse_readdir(const char * path, void * buf,
		 fuse_fill_dir_t filler, off_t ofs, struct fuse_file_info * fi)
{
    error_t err = -ENOENT;
    trace_readdir("%s ofs=%"PRId64, path, ofs);
    tree_lock();

    fuse_node_t fnode = fnode_lookup(CTX->fuse_node_root, path);
    if (fnode) {
	if (!S_ISDIR(fnode->inode->i_mode))
	    err = -ENOTDIR;
	else
	    err = fnode_readdir(fnode, buf, filler, ofs);
    }

    tree_unlock();
    return err;
}

static error_t
op_fuse_open(const char * path, struct fuse_file_info * fi)
{
    error_t err = -ENOENT;
    trace_fs("%s", path);

    tree_lock();

    fuse_node_t fnode = fnode_lookup(CTX->fuse_node_root, path);
    if (fnode) {
	if (S_ISDIR(fnode->inode->i_mode)) {
	    tree_unlock();
	    err = -EISDIR;
	} else {
	    struct file * file;
	    node_hold(fnode);
	    tree_unlock();
	    file = vzalloc(sizeof(*file));
	    file->inode = fnode->inode;
	    fi->fh = (uintptr_t)file;	/* stash file pointer in fuse info */
	    err = fnode_open(fi);
	    if (err) {
		pr_warning("fnode[%s]->proc_fops->open returned %d\n",
				fnode->name, err);
		vfree(file);
		fi->fh = (uintptr_t)NULL;
		node_drop(fnode);
	    }
	}
    } else {
	tree_unlock();
    }

    if (!err && !S_ISBLK(fnode->inode->i_mode)) {
	fi->nonseekable = true;
	fi->direct_io = true;
    }

    return err;
}

static error_t
op_fuse_release(const char * path, struct fuse_file_info * fi)
{
    error_t err = -EINVAL;
    trace_fs("%s", path);

    fuse_node_t fnode = FI_FNODE(fi);

    if (!FLAG_NOPATH)
	assert_eq(fnode, ({ fuse_node_t foo = fuse_node_lookup(path); foo; }));

    if (fnode) {
	if (!S_ISDIR(fnode->inode->i_mode)) {
	    err = fnode_release(fi);
	    if (err)
		pr_warning("fnode[%s]->proc_fops->release got %d\n",
				    fnode->name, err);
	    else {
		node_drop(fnode);
		vfree((void *)fi->fh);
		fi->fh = (uintptr_t)NULL;
	    }
	}
    }

    return err;
}

/* For op_fuse_read, _write, and _fsync the fnode was held by _open */
static int /*ssize_t?*/
op_fuse_read(const char * path, char * buf, size_t size, off_t ofs, struct fuse_file_info * fi)
{
    ssize_t ret = -EBADF;
    buf[0] = '\0';
    trace_fs("%s size=%lu ofs=%"PRId64, path, size, ofs);

    fuse_node_t fnode = FI_FNODE(fi);

    if (!FLAG_NOPATH)
	assert_eq(fnode, ({ fuse_node_t foo = fuse_node_lookup(path); foo; }));

    if (fnode) {
	assert_node_held(fnode);
	if (S_ISDIR(fnode->inode->i_mode))
	    ret = -EISDIR;
	else
	    ret = fnode_read(fi, buf, size, ofs);

	if (ret >= 0)
	    fnode->inode->i_atime = time(NULL);
    }

    trace_fs("READ %s REPLY len=%"PRId64, path, ret);
    return (int)ret;
}

static int /*ssize_t?*/
op_fuse_write(const char * path, const char * buf, size_t size, off_t ofs, struct fuse_file_info * fi)
{
    ssize_t ret = -EBADF;
    trace_fs("%s size=%lu ofs=%"PRId64, path, size, ofs);

    fuse_node_t fnode = FI_FNODE(fi);

    if (!FLAG_NOPATH)
	assert_eq(fnode, ({ fuse_node_t foo = fuse_node_lookup(path); foo; }));

    if (fnode) {
	assert_node_held(fnode);
	if (S_ISDIR(fnode->inode->i_mode))
	    ret = -EISDIR;
	else
	    ret = fnode_write(fi, buf, size, ofs);

	if (ret >= 0)
	    fnode->inode->i_mtime = time(NULL);
    }

    trace_fs("WRITE %s REPLY len=%"PRId64, path, ret);
    return (int)ret;
}

static error_t
op_fuse_fsync(const char * path, int datasync, struct fuse_file_info * fi)
{
    error_t err = -EBADF;
    trace_fs("FSYNC %s %d", path, datasync);

    fuse_node_t fnode = FI_FNODE(fi);

    if (!FLAG_NOPATH)
	assert_eq(fnode, ({ fuse_node_t foo = fuse_node_lookup(path); foo; }));

    if (fnode) {
	assert_node_held(fnode);
	if (S_ISDIR(fnode->inode->i_mode))
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
fuse_tree_init(const char * mountpoint)
{
    size_t namesize;
    assert(mountpoint);

    if (*mountpoint != '/')
	return -EINVAL;

    const char * rootname = 1 + strrchr(mountpoint, '/');
    if (*rootname == '\0')
	return -EINVAL;

    assert_eq(CTX, NULL);
    CTX = vzalloc(sizeof(*CTX));
    mutex_init(&CTX->lock);
    namesize = 1 + strlen(mountpoint);
    CTX->mountpoint = vmalloc(namesize);
    memcpy(CTX->mountpoint, mountpoint, namesize);

    /* Create the root fnode that overlays the mount point */
    CTX->fuse_node_root = _fnode_create(rootname, S_IFDIR|0555, NULL, 0);

    fnode_check(CTX->fuse_node_root);
    return 0;
}

error_t
fuse_tree_exit(void)
{
    fuse_node_t root;
    if (!CTX)
	return -EINVAL;

    root = CTX->fuse_node_root;
    assert(root);
    assert(CTX->mountpoint);

    if (fnode_nchild(root)) {
	char * str;
	pr_warning("fuse root fnode[%s] still has %d child(ren) e.g. '%s'\n",
	    root->name, fnode_nchild(root), root->child->name);
	str = fuse_tree_fmt();
	pr_warning("Exit with fuse tree nodes still existing:\n%s\n", str);
	vfree(str);
	return -EBUSY;
    }

    node_drop(root);
    vfree(CTX->mountpoint);
    vfree(CTX);
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
	int rc;
	char * cmd;
	cmd = kasprintf(0, "/bin/mkdir -p %s; chmod 777 %s",
				    CTX->mountpoint, CTX->mountpoint);
	rc = UMC_system(cmd);
	if (rc)
	    pr_warning("'%s' returns %d\n", cmd, rc);
	else
	    pr_notice("created fuse root %s\n", CTX->mountpoint);
	vfree(cmd);
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

	"-o", "auto_cache",	    /* invalidate kernel cache on each open */

//	"-o", "sync_read",	    /* perform all reads synchronously */
//	"-o", "sync",		    /* perform all I/O synchronously */
//	"-o", "max_readahead=0",    /* max bytes to read-ahead */

	"-o", "atomic_o_trunc",	    /* avoid calls to truncate */
	"-o", "default_permissions",/* fuse do mode permission checking */

//	"-o", "dev",		    /* allow device nodes */
				    // XXX interpreted as KERNEL dev_t!

	NULL			    /* end of argv[] list */
    };

    int argc = sizeof(argv)/sizeof(argv[0]) - 1/*NULL*/;

    nlprintk("Calling fuse loop on tid=%u", gettid());
    err = fuse_main(argc, argv, &fnode_ops, NULL);
    nlprintk("Returned err=%d from fuse loop on tid=%u", err, gettid());
    return err;
}
