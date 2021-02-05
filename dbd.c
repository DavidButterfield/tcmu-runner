/* dbd.c -- tcmu-runner driver for Distributed Block Device
 *
 * Copyright 2017-2021 David A. Butterfield
 * MIT License [SPDX:MIT https://opensource.org/licenses/MIT]
 *
 * This tcmu-runner backstore handler interfaces to a Distributed Block Device
 * which provides mirror redundancy across multiple host computers.
 */
#define _GNU_SOURCE
#include <sys/types.h>
#include <errno.h>
#include <sys/uio.h>

#include "tcmu-runner.h"
#include "tcmur_device.h"

#define BLOCK_SIZE (4*1024)

typedef struct tcmu_dbd {
    size_t		size;
    void	      * private;
} * tcmu_dbd;

// #include "_cgo_export.h"
extern ssize_t go_dbd_probe(void *);
extern ssize_t go_dbd_read(struct iovec *, size_t, size_t, off_t);
extern ssize_t go_dbd_write(struct iovec *, size_t, size_t, off_t);

static int tcmu_dbd_read(struct tcmu_device *td, struct tcmur_cmd *cmd,
	  struct iovec *iov, size_t niov, size_t size, off_t seekpos)
{
    ssize_t ret;
    tcmu_dbd dbd = tcmur_dev_get_private(td);

    if (seekpos >= dbd->size) {
	tcmu_dev_err(td, "read seekpos out of range 0x%lx\n", seekpos);
	return TCMU_STS_RANGE;
    }

    if (seekpos + size > dbd->size)
	size = dbd->size - seekpos;

    ret = go_dbd_read(iov, niov, size, seekpos);
    if (ret != size) {
	tcmu_dev_err(td, "read returned incorrect size 0x%lx/0x%lx\n", ret, size);
	return TCMU_STS_RD_ERR;
    }

    return TCMU_STS_OK;
}

static int tcmu_dbd_write(struct tcmu_device *td, struct tcmur_cmd *cmd,
	   struct iovec *iov, size_t niov, size_t size, off_t seekpos)
{
    ssize_t ret;
    tcmu_dbd dbd = tcmur_dev_get_private(td);

    if (seekpos >= dbd->size) {
	tcmu_dev_err(td, "write seekpos out of range 0x%lx\n", seekpos);
	return TCMU_STS_RANGE;
    }

    if (seekpos + size > dbd->size)
	size = dbd->size - seekpos;

    ret = go_dbd_write(iov, niov, size, seekpos);
    if (ret != size) {
	tcmu_dev_err(td, "write returned incorrect size 0x%lx/0x%lx\n", ret, size);
	return TCMU_STS_WR_ERR;
    }

    return TCMU_STS_OK;
}

static int tcmu_dbd_flush(struct tcmu_device *td, struct tcmur_cmd *cmd)
{
    /*
    tcmu_dbd dbd = tcmur_dev_get_private(td);

    if (dbd_sync(dbd->private) < 0)
	return TCMU_STS_WR_ERR;
    */

    return TCMU_STS_OK;
}

static void tcmu_dbd_close(struct tcmu_device *td)
{
    tcmu_dbd dbd = tcmur_dev_get_private(td);

    /*
    if (dbd_close(dbd->private) < 0)
	return
    */

    tcmur_dev_set_private(td, NULL);

    free(dbd);
}

static int tcmu_dbd_open(struct tcmu_device * td, bool reopen)
{
    tcmu_dbd dbd;
    char *config = tcmu_dev_get_cfgstring(td);
    tcmu_dev_dbg(td, "tcmu_dbd_open config %s\n", config);

    tcmu_dev_set_block_size(td, BLOCK_SIZE);

    dbd = calloc(1, sizeof(*dbd));
    if (!dbd) {
	tcmu_dev_err(td, "%s: cannot allocate state\n", config);
	return -ENOMEM;
    }

    dbd->private = NULL;    //XXX
    dbd->size = go_dbd_probe(dbd->private);
    if (dbd->size <= 0) {
	free(dbd);
	return -EIO;
    }

    tcmu_dev_set_num_lbas(td, dbd->size / tcmu_dev_get_block_size(td));
    tcmu_dev_info(td, "%s: size determined as %lu\n", config, dbd->size);

    tcmur_dev_set_private(td, dbd);
    
    tcmu_dev_dbg(td, "config %s, size %ld\n", config, dbd->size);
    return 0;
}

static const char tcmu_dbd_cfg_desc[] =
    "YOUR MESSAGE HERE!\n";

struct tcmur_handler tcmu_dbd_handler = {
    .name	   = "Distributed Block Device",
    .subtype       = "dbd",
    .cfg_desc      = tcmu_dbd_cfg_desc,
    .open	   = tcmu_dbd_open,
    .close	   = tcmu_dbd_close,
    .read	   = tcmu_dbd_read,
    .write	   = tcmu_dbd_write,
    .flush	   = tcmu_dbd_flush,
    .nr_threads    = 1, /* implies op completes before return from callout */
};

int handler_init(void)
{
    return tcmur_register_handler(&tcmu_dbd_handler);
}
