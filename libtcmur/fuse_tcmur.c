/* fuse_tcmur.c -- translate file_operations into calls to tcmu-runner handlers
 *
 * Copyright 2019 David A. Butterfield
 *
 * This file is licensed to you under your choice of the GNU Lesser
 * General Public License, version 2.1 or any later version (LGPLv2.1 or
 * later), or the Apache License 2.0.
 *
 * The functions below translate file_operations into tcmur_*() calls, and
 * _STS_ replies into -errno.
 */
#define _GNU_SOURCE
#include <sys/types.h>
#include <inttypes.h>
#include <errno.h>

#include "libtcmur.h"
#include "fuse_tree.h"
#include "fuse_tcmur.h"

#define trace_ioerr(fmtargs...)	        nlprintk(fmtargs)

/***** Ferry an op from fuse to libtcmur and back *****/
struct fuse_tcmur_op {
    struct libtcmur_task    tcmur_task;
    int			    minor;
    struct completion	    complete;
    tcmur_status_t	    sts;
    struct iovec	    iov[1];	/* we get singleton bufs from fuse */
};

#define op_of_cmd(cmd) container_of((cmd), struct fuse_tcmur_op, tcmur_task.tcmur_cmd)

/* Callback from the tcmur handler */
static void
io_done(struct tcmu_device * tcmu_dev, struct tcmur_cmd * cmd, tcmur_status_t sts)
{
    struct fuse_tcmur_op * op = op_of_cmd(cmd);
    if (sts)
	trace_ioerr("tcmur[%d] OP completes with sts=%d\n", op->minor, op->sts);
    op->sts = sts;
    complete(&op->complete);
}

/* Wait for io_done to be called, and translate status to errno */
static ssize_t
io_wait(struct fuse_tcmur_op * op, size_t success)
{
    wait_for_completion(&op->complete);

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
    init_completion(&op->complete);

    struct tcmur_cmd * cmd = &op->tcmur_task.tcmur_cmd;
    cmd->iovec = op->iov;
    cmd->iov_cnt = 1;
    cmd->done = io_done;
}

/***** Synchronous file_operations called from fuse_tree.c *****/

static ssize_t
dev_read(struct file * file, void * buf, size_t iosize, off_t *lofsp)
{
    int minor = (int)(uintptr_t)file_pde_data(file);
    ssize_t err;
    struct fuse_tcmur_op op;
    memset(&op, 0, sizeof(op));

    op_setup(&op, minor, buf, iosize);

    err = tcmur_read(minor, &op.tcmur_task, op.tcmur_task.tcmur_cmd.iovec,
		    	     op.tcmur_task.tcmur_cmd.iov_cnt, iosize, *lofsp);
    if (err)
	return err;

    *lofsp += (off_t)iosize;		    //XXX needed?

    return io_wait(&op, iosize);
}

static ssize_t
dev_write(struct file * file, const char * buf, size_t iosize, off_t *lofsp)
{
    int minor = (int)(uintptr_t)file_pde_data(file);
    ssize_t err;
    struct fuse_tcmur_op op;
    memset(&op, 0, sizeof(op));

    op_setup(&op, minor, buf, iosize);

    err = tcmur_write(minor, &op.tcmur_task, op.tcmur_task.tcmur_cmd.iovec,
		    	      op.tcmur_task.tcmur_cmd.iov_cnt, iosize, *lofsp);
    if (err)
	return err;

    *lofsp += (off_t)iosize;		    //XXX needed?

    return io_wait(&op, iosize);
}

static error_t
dev_sync(struct file * file, int datasync)
{
    int minor = (int)(uintptr_t)file_pde_data(file);
    error_t err;
    struct fuse_tcmur_op op;
    memset(&op, 0, sizeof(op));

    op_setup(&op, minor, NULL, 0);

    err = tcmur_flush(minor, &op.tcmur_task);
    if (err)
	return err;

    return (error_t)io_wait(&op, 0);
}

/* fuse ops for nodes representing tcmu-runner handler devices */
static struct file_operations dev_fops = {
    .read = dev_read,
    .write = dev_write,
    .fsync = dev_sync,
};

error_t
fuse_tcmur_init(int major, int max_minors)
{
    return fuse_tcmur_ctl_init(&dev_fops);
}

error_t
fuse_tcmur_exit(void)
{
    return fuse_tcmur_ctl_exit();
}
