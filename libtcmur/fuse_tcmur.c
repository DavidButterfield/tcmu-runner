/* fuse_tcmur.c -- translate fuse_node_ops into calls to tcmu-runner handlers
 *
 * Copyright 2019 David A. Butterfield
 *
 * This file is licensed to you under your choice of the GNU Lesser
 * General Public License, version 2.1 or any later version (LGPLv2.1 or
 * later), or the Apache License 2.0.
 *
 * Main program uses fuse_tree and libtcmur to provide mountable access to
 * tcmu-runner devices.
 *
 * The functions below translate fuse_node_ops into tcmur_*() calls, and
 * _STS_ replies into -errno.
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

#include "libtcmur.h"
#include "fuse_tree.h"
#include "fuse_tcmur.h"
#include "sys_misc.h"

#define trace(fmtargs...)       //  sys_msg(fmtargs)

/***** Ferry an op from fuse to libtcmur and back *****/
struct fuse_tcmur_op {
    struct tcmulib_cmd	    cmd;
    int			    minor;
    struct sys_completion   complete;
    tcmur_status_t	    sts;
    struct iovec	    iov[1];	/* we get singleton bufs from fuse */
};

#define op_of_cmd(cmd) container_of((cmd), struct fuse_tcmur_op, cmd)

/* Callback from the tcmur handler */
static void
io_done(struct tcmu_device * tcmu_dev, struct tcmulib_cmd * cmd, tcmur_status_t sts)
{
    struct fuse_tcmur_op * op = op_of_cmd(cmd);
    if (sts) {
	trace("tcmur[%d] OP completes with sts=%d\n", op->minor, op->sts);
    }
    op->sts = sts;
    sys_complete(&op->complete);
}

/* Wait for io_done to be called, and translate status to errno */
static ssize_t
io_wait(struct fuse_tcmur_op * op, size_t success)
{
    sys_wait_for_completion(&op->complete);

    /* Translate STS into -errno */
    if (op->sts != TCMU_STS_OK)
	return -EIO;

    return (ssize_t)success;
}

/* Initialize an op */
static inline void
op_setup(struct fuse_tcmur_op * op, int minor, const void * buf, size_t iosize)
{
    memset(op, 0, sizeof(*op));
    op->iov[0].iov_base = _unconstify(buf);
    op->iov[0].iov_len = iosize;
    op->minor = minor;
    sys_completion_init(&op->complete);

    struct tcmulib_cmd * cmd = &op->cmd;
    cmd->iovec = op->iov;
    cmd->iov_cnt = 1;
    cmd->done = io_done;
}

/***** Synchronous fuse_node_ops called from fuse_tree.c *****/

static ssize_t
dev_read(uintptr_t minor_arg, void * buf, size_t iosize, loff_t lofs)
{
    int minor = (int)minor_arg;
    ssize_t err;
    struct fuse_tcmur_op op;
    memset(&op, 0, sizeof(op));

    op_setup(&op, minor, buf, iosize);

    err = tcmur_read(minor, &op.cmd, op.cmd.iovec, op.cmd.iov_cnt, iosize, lofs);
    if (err)
	return err;

    return (ssize_t)io_wait(&op, iosize);
}

static ssize_t
dev_write(uintptr_t minor_arg, const char * buf, size_t iosize, loff_t lofs)
{
    int minor = (int)minor_arg;
    ssize_t err;
    struct fuse_tcmur_op op;
    memset(&op, 0, sizeof(op));

    op_setup(&op, minor, buf, iosize);

    err = tcmur_write(minor, &op.cmd, op.cmd.iovec, op.cmd.iov_cnt, iosize, lofs);
    if (err)
	return err;

    return io_wait(&op, iosize);
}

static error_t
dev_sync(uintptr_t minor_arg, int datasync)
{
    int minor = (int)minor_arg;
    error_t err;
    struct fuse_tcmur_op op;
    memset(&op, 0, sizeof(op));

    op_setup(&op, minor, NULL, 0);

    err = tcmur_flush(minor, &op.cmd);
    if (err)
	return err;

    return (error_t)io_wait(&op, 0);
}

/* fuse ops for nodes representing tcmu-runner handler devices */
static struct fuse_node_ops dev_fops = {
    .read = dev_read,
    .write = dev_write,
    .fsync = dev_sync,
};

error_t
fuse_tcmur_init(void)
{
    return fuse_tcmur_ctl_init(&dev_fops);
}

error_t
fuse_tcmur_exit(void)
{
    return fuse_tcmur_ctl_exit();
}

#ifdef WANT_MAIN

/* For failure (non-zero), prefers a -errno from FN */
#define DO_OR_DIE(FN) \
do { \
    error_t _err = (FN); \
    if (_err) { \
	char * str = fuse_tree_fmt(); \
	sys_error("'%s' err=%d %s\n%s\n", \
		    #FN, _err, _err < 0 ? strerror(-_err) : "", str); \
	exit(1); \
    } \
} while (0)

#define DO_OR_WARN(FN) \
do { \
    error_t _err = (FN); \
    if (_err) { \
	char * str = fuse_tree_fmt(); \
	sys_warning("'%s' err=%d %s\n%s\n", \
		    #FN, _err, _err < 0 ? strerror(-_err) : "", str); \
    } \
} while (0)

#define DEFAULT_FUSE_TCMUR_MOUNTPOINT "/tcmur"
static const char * mountpoint = DEFAULT_FUSE_TCMUR_MOUNTPOINT;
static const char * handler_prefix = DEFAULT_HANDLER_PATH "/handler_";

//XXX Add command-line options to set mountpoint and handler_prefix
int main(int argc, char * argv[])
{
    fuse_node_t fnode_sys;

    DO_OR_DIE(fuse_tree_init(mountpoint));	/* prepare fuse */

    DO_OR_DIE(libtcmur_init(handler_prefix));	/* prepare libtcmur */

    /* create /dev, /sys/module */
    DO_OR_DIE(!(             fuse_tree_mkdir("dev", NULL) != 0));
    DO_OR_DIE(!((fnode_sys = fuse_tree_mkdir("sys", NULL)) != 0));
    DO_OR_DIE(!(             fuse_tree_mkdir("module", fnode_sys) != 0));

    DO_OR_DIE(fuse_tcmur_init());		/* init ctl_fops */

    DO_OR_WARN(fuse_loop_run(NULL));		/* run fuse_main() */

    sleep(1);

    DO_OR_WARN(fuse_tcmur_exit());		/* stop cmdline processing */

    /* remove /dev, /sys/module if empty */
    DO_OR_WARN(fuse_tree_rmdir("dev", NULL));
    DO_OR_WARN(fuse_tree_rmdir("module", fnode_sys));
    DO_OR_WARN(fuse_tree_rmdir("sys", NULL));

    /* -EBUSY if handler(s) still loaded */
    DO_OR_DIE(libtcmur_exit());			/* free libtcmur context */

    DO_OR_WARN(fuse_tree_exit());		/* free fuse_tree context */

    return 0;
}

#endif /* WANT_MAIN */
