/* libtcmur.c -- Usermode API to tcmu-runner block storage handlers
 *
 * Copyright (c) 2019 David A. Butterfield
 *
 * This file is licensed to you under your choice of the GNU Lesser
 * General Public License, version 2.1 or any later version (LGPLv2.1 or
 * later), or the Apache License 2.0.
 *
 * libtcmur provides a usermode application programming interface to access
 * block storage services through tcmu-runner block storage handlers.
 *
 * LIO, TCMU and tcmu-runner are not involved -- this only uses the tcmu-runner
 * loadable storage handlers, e.g. qcow, glfs, ram, etc.
 *
 * This library makes use of the handler read, write, and flush block I/O
 * functions only -- no calls are made to handle_cmd().
 */
#define _GNU_SOURCE
#include <sys/types.h>
#include <inttypes.h>
#include <stdbool.h>
#include <errno.h>
#include <stddef.h>
#include <unistd.h>
#include <stdlib.h>
#include <ctype.h>
#include <dlfcn.h>

#include "libtcmur.h"
#include "sys_assert.h" /* include after tcmu-runner.h (libtcmur.h) */

#include <string.h>	/* include after sys_impl.h (libtcmur.h) */

#define tcmu_io_trace_dev(dev, fmtargs...)  //  tcmu_dev_info(dev, "libtcmur: "fmtargs)

/* Print up to two stacktraces of callers per stubbed function */
#define STUB_WARN() do { \
    static int been_here = 0; \
    if (been_here++ < 2) \
	sys_backtrace("UNEXPECTED CALL TO %s", __func__); \
} while (0)

/* Handlers may have code that calls these functions, even though that code
 * would only ever be executed if we called handle_command(), which we don't.
 * But it is unexpected so we want to know if and whence they're ever called.
 */

/* SCSI-specific */
extern int tcmur_handle_caw(struct tcmu_device *, struct tcmur_cmd *, void * fn);
extern int tcmur_handle_writesame(struct tcmu_device *, struct tcmur_cmd *, void * fn);
extern void tcmu_notify_lock_lost(struct tcmu_device *);

uint32_t tcmu_dev_get_opt_unmap_gran(struct tcmu_device *dev) { STUB_WARN(); return 0; }
bool tcmu_dev_get_unmap_enabled(struct tcmu_device *dev) { STUB_WARN(); return false; }
uint32_t tcmu_dev_get_unmap_gran_align(struct tcmu_device *dev) { STUB_WARN(); return 0; }
void tcmu_dev_set_max_unmap_len(struct tcmu_device *dev, uint32_t len) { STUB_WARN(); }
void tcmu_dev_set_opt_unmap_gran(struct tcmu_device *dev, uint32_t len, bool split) { STUB_WARN(); }
void tcmu_dev_set_opt_xcopy_rw_len(struct tcmu_device *dev, uint32_t len) { STUB_WARN(); }
char * tcmu_cfgfs_dev_get_wwn(struct tcmu_device *dev) { STUB_WARN(); return NULL; }
int tcmur_handle_caw(struct tcmu_device *dev, struct tcmur_cmd *cmd, void * fn) { STUB_WARN(); return -1; }
int tcmur_handle_writesame(struct tcmu_device *dev, struct tcmur_cmd *cmd, void * fn) { STUB_WARN(); return -1; }
void tcmu_notify_lock_lost(struct tcmu_device *dev) { STUB_WARN(); }

/* Possibly applicable to libtcmur (XXX possible enhancement) */
extern void tcmu_notify_conn_lost(struct tcmu_device *);
extern int tcmur_dev_update_size(struct tcmu_device *, uint64_t new_size);

uint64_t tcmu_cfgfs_dev_get_info_u64(struct tcmu_device *dev, const char *name, int *fn_ret) { STUB_WARN(); return 0; }
int tcmu_make_absolute_logfile(char *path, const char *filename) { STUB_WARN(); return -1; }
bool tcmu_dev_get_solid_state_media(struct tcmu_device *dev) { STUB_WARN(); return false; }
void tcmu_dev_set_solid_state_media(struct tcmu_device *dev, bool solid_state) { STUB_WARN(); }
void tcmu_notify_conn_lost(struct tcmu_device *dev) { STUB_WARN(); }
int tcmur_dev_update_size(struct tcmu_device *dev, uint64_t new_size) { STUB_WARN(); return -1; }

/******************************************************************************/

const char * libtcmur_version = "libtcmur " TCMUR_VERSION;

static const char * handler_prefix = DEFAULT_HANDLER_PATH "/handler_";
static struct tcmur_handler * the_tcmu_handlers[MAX_TCMUR_HANDLERS];
static struct tcmu_device * the_tcmu_devices[MAX_TCMUR_MINORS];

#define device_of_minor(minor) \
  ((minor) < (int)ARRAY_SIZE(the_tcmu_devices) ? the_tcmu_devices[minor] : NULL)

struct tcmu_device {
	uint64_t num_lbas;
	uint32_t block_size;
	uint32_t max_xfer_len;
	unsigned int write_cache_enabled:1;
	unsigned int solid_state_media:1;
	char dev_name[16];		    /* e.g. "qcow3" */
	char cfgstring[PATH_MAX];
	char cfgstring_orig[PATH_MAX];
	void *hm_private;
	struct tcmur_handler *rhandler;
};

static int
minor_of_devname(const char * devname)
{
    int minor;
    for (minor = 0; minor < MAX_TCMUR_MINORS; minor++) {
	struct tcmu_device * dev = device_of_minor(minor);
	if (dev && !strcmp(dev->dev_name, devname))
	    return minor;
    }
    return -1;
}

/* Find handler whose subtype string matches the subtype argument,
 * and pass back its slot number
 */
static struct tcmur_handler *
tcmur_find_handler_slot(const char * subtype, int *slotp)
{
	int i;
	for (i = 0; i < (int)ARRAY_SIZE(the_tcmu_handlers); i++) {
		struct tcmur_handler * h = the_tcmu_handlers[i];
		if (h)
		if (h && !strcmp(h->subtype, subtype)) {
			*slotp = i;
			return h;
		}
	}
	*slotp = -1;
	return NULL;
}

/* Find handler whose subtype string matches the subtype argument */
static struct tcmur_handler *
tcmur_find_handler(const char * subtype)
{
	int slot;
	return tcmur_find_handler_slot(subtype, &slot);
}

/* Return the handler that corresponds to cfgstr */
static struct tcmur_handler *
handler_of_cfgstr(const char * cfg)
{
    struct tcmur_handler * handler;
    char * hname;
    const char * subtype = cfg;
    const char * p;

    while (*subtype == '/')
	subtype++;
    p = subtype;
    while (isalnum(*p))
	p++;

    hname = kasprintf(0, "%.*s", (int)(p - subtype), subtype);

    handler = tcmur_find_handler(hname);
    vfree(hname);
    return handler;
}

/* Call a handler's check_config function */
error_t
tcmur_check_config(const char * cfg)
{
    error_t err;
    char * reason = NULL;
    struct tcmur_handler * handler;
    struct tcmu_device * dev = NULL;

    if (!cfg || *cfg != '/') {
	tcmu_err("config string must start with '/': '%s'\n", cfg);
	return -EINVAL;
    }

    if (strlen(cfg) >= sizeof(dev->cfgstring)) {
	tcmu_err("cfg string too long (%lu/%lu): '%s'\n",
		 strlen(cfg), sizeof(dev->cfgstring)-1, cfg);
	return -EINVAL;
    }

    handler = handler_of_cfgstr(cfg);

    if (!handler)
	return -ENXIO;	    /* no handler subtype matches first cfg segment */

    if (!handler->check_config)
	return 0;	    /* OK, no check function */

    /* Advance over handler_name to the handler-specific cfg string */
    cfg = strchrnul(cfg+1, '/');

    err = handler->check_config(cfg, &reason);
    if (err) {
	tcmu_warn("handler %s failed check_config(%s) reason: %s\n",
		    handler->name, cfg, reason?:"none");
	if (reason)
	    vfree(reason);
    }

    return err;
}

/******** Handlers call these functions ********/

/* Returns zero on success, or -1 on failure */
int
tcmur_register_handler(struct tcmur_handler *handler)
{
	int i;
	int empty_slot = -1;

	for (i = 0; i < (int)ARRAY_SIZE(the_tcmu_handlers); i++) {
		struct tcmur_handler * h = the_tcmu_handlers[i];
		if (!h) {
			if (empty_slot < 0)
				empty_slot = i;
		} else if (!strcmp(h->subtype, handler->subtype)) {
			tcmu_err("Handler %s has already been registered\n",
				 handler->subtype);
			return -1;
		}
	}

	assert_ge(empty_slot, 0);   /* checked before handler_init() */

	tcmu_info("Handler %s registered, slot=%d\n",
		    handler->subtype, empty_slot);
	the_tcmu_handlers[empty_slot] = handler;
	return 0;
}

bool
tcmur_unregister_handler(struct tcmur_handler *handler)
{
	int i;
	for (i = 0; i < (int)ARRAY_SIZE(the_tcmu_handlers); i++) {
		if (the_tcmu_handlers[i] == handler) {
			tcmu_info("Handler %s unregistered, slot=%d\n",
				    the_tcmu_handlers[i]->subtype, i);
			the_tcmu_handlers[i] = NULL;
			return true;
		}
	}
	tcmu_info("Handler %s could not be unregistered, not found\n",
		    handler->subtype);
	return false;
}

extern void *tcmur_dev_get_private(struct tcmu_device *);
void *tcmur_dev_get_private(struct tcmu_device *dev)
{
	return dev->hm_private;
}

extern void tcmur_dev_set_private(struct tcmu_device *, void *);
void tcmur_dev_set_private(struct tcmu_device *dev, void *private)
{
	dev->hm_private = private;
}

void tcmu_dev_set_num_lbas(struct tcmu_device *dev, uint64_t num_lbas)
{
	dev->num_lbas = num_lbas;
}

uint64_t tcmu_dev_get_num_lbas(struct tcmu_device *dev)
{
	return dev->num_lbas;
}

void tcmu_dev_set_block_size(struct tcmu_device *dev, uint32_t block_size)
{
	dev->block_size = block_size;
}

uint32_t tcmu_dev_get_block_size(struct tcmu_device *dev)
{
	return dev->block_size;
}

void tcmu_dev_set_max_xfer_len(struct tcmu_device *dev, uint32_t len)
{
	dev->max_xfer_len = len;    /* in units of block_size */
}

uint32_t tcmu_dev_get_max_xfer_len(struct tcmu_device *dev)
{
	return dev->max_xfer_len;
}

void tcmu_dev_set_write_cache_enabled(struct tcmu_device *dev, bool enabled)
{
	dev->write_cache_enabled = enabled;
}

bool tcmu_dev_get_write_cache_enabled(struct tcmu_device *dev)
{
	return dev->write_cache_enabled;
}

char * tcmu_dev_get_cfgstring(struct tcmu_device *dev)
{
	return dev->cfgstring;
}

void tcmur_cmd_complete(struct tcmu_device *dev, void *data, int sts)
{
    struct tcmur_cmd *tcmur_cmd = data;
    tcmur_cmd->done(dev, tcmur_cmd, sts);
}

/******** Client calls these functions to invoke handlers ********/

/* tcmur_read(), tcmur_write() and tcmur_flush() start I/O operations to the
 * specified minor.  Errors in the I/O start process are reported by a -errno
 * return from these calls.  A return value of zero denotes a successful I/O
 * start, in which case there will be a later completion call to cmd->done(),
 * which may report either an "sts" error, or success (denoted by TCMU_STS_OK).
 */

error_t
tcmur_read(int minor, struct tcmur_cmd * cmd,
	    struct iovec * iov, size_t niov, size_t nbyte, off_t seekpos)
{
    off_t endpos = seekpos + (ssize_t)nbyte;
    struct tcmu_device * dev = device_of_minor(minor);
    error_t err = 0;
    tcmur_status_t sts;

    if (!dev)
	return -ENODEV;	    /* nonexistent minor */
    if (!dev->rhandler->read)
	return -ENXIO;	    /* handler has no read function */
    if (endpos < seekpos)
	return -EINVAL;	    /* I/O exceeding device bounds */
    if (endpos > (off_t)(dev->num_lbas * dev->block_size))
	return -EINVAL;	    /* I/O exceeding device bounds */

    tcmu_io_trace_dev(dev, "READ %lu bytes at offset %lu\n", nbyte, seekpos);

    /* XXX Do handlers look at these, or only at the passed arguments? */
    cmd->iovec = iov;
    cmd->iov_cnt = niov;

    sts = dev->rhandler->read(dev, cmd, iov, niov, nbyte, seekpos);
    if (sts == TCMU_STS_OK) {
	//XXX should let our caller do this
	if (dev->rhandler->nr_threads)
	    tcmur_cmd_complete(dev, cmd, sts);
    } else {
	tcmu_io_trace_dev(dev, "READ ERROR sts=%d", sts);
	if (sts == TCMU_STS_NO_RESOURCE)
	    err = -ENOMEM;
	else
	    err = -EIO;
    }

    return err;
}

error_t
tcmur_write(int minor, struct tcmur_cmd * cmd,
	    struct iovec * iov, size_t niov, size_t nbyte, off_t seekpos)
{
    off_t endpos = seekpos + (ssize_t)nbyte;
    struct tcmu_device * dev = device_of_minor(minor);
    error_t err = 0;
    tcmur_status_t sts;

    if (!dev)
	return -ENODEV;	    /* nonexistent minor */
    if (!dev->rhandler->write)
	return -ENXIO;	    /* handler has no write function */
    if (endpos < seekpos)
	return -EINVAL;	    /* I/O exceeding device bounds */
    if (endpos > (off_t)(dev->num_lbas * dev->block_size))
	return -EINVAL;	    /* I/O exceeding device bounds */

    tcmu_io_trace_dev(dev, "WRITE %lu bytes at offset %lu\n", nbyte, seekpos);

    /* XXX Do handlers look at these, or only at the passed arguments? */
    cmd->iovec = iov;
    cmd->iov_cnt = niov;

    sts = dev->rhandler->write(dev, cmd, iov, niov, nbyte, seekpos);
    if (sts == TCMU_STS_OK) {
	//XXX should let our caller do this
	if (dev->rhandler->nr_threads)
	    tcmur_cmd_complete(dev, cmd, sts);
    } else {
	tcmu_io_trace_dev(dev, "WRITE ERROR sts=%d", sts);
	if (sts == TCMU_STS_NO_RESOURCE)
	    err = -ENOMEM;
	else
	    err = -EIO;
    }

    return err;
}

error_t
tcmur_flush(int minor, struct tcmur_cmd * cmd)
{
    struct tcmu_device * dev = device_of_minor(minor);
    error_t err = 0;
    tcmur_status_t sts;

    if (!dev)
	return -ENODEV;	    /* nonexistent minor */
    if (!dev->rhandler->flush)
	return 0;	    //XXX right?

    tcmu_io_trace_dev(dev, "flush\n");

    sts = dev->rhandler->flush(dev, cmd);
    if (sts == TCMU_STS_OK) {
	//XXX should let our caller do this
	if (dev->rhandler->nr_threads)
	    tcmur_cmd_complete(dev, cmd, sts);
    } else {
	tcmu_io_trace_dev(dev, "FLUSH ERROR sts=%d", sts);
	if (sts == TCMU_STS_NO_RESOURCE)
	    err = -ENOMEM;
	else
	    err = -EIO;
    }

    return err;
}

/******** Client calls these functions to manage devices and handlers ********/

const char *
tcmur_get_dev_name(int minor)
{
    struct tcmu_device * dev = device_of_minor(minor);
    if (!dev)
	return NULL;
    return dev->dev_name;
}

ssize_t
tcmur_get_size(int minor)
{
    struct tcmu_device * dev = device_of_minor(minor);
    if (!dev)
	return -ENODEV;	    /* nonexistent minor */
    return (ssize_t)(dev->num_lbas * dev->block_size);
}

ssize_t
tcmur_get_block_size(int minor)
{
    struct tcmu_device * dev = device_of_minor(minor);
    if (!dev)
	return -ENODEV;	    /* nonexistent minor */
    return dev->block_size;
}

ssize_t
tcmur_get_max_xfer(int minor)
{
    struct tcmu_device * dev = device_of_minor(minor);
    if (!dev)
	return -ENODEV;	    /* nonexistent minor */
    return dev->max_xfer_len;
}

/* Lookup and hold the device -- returns a minor number or -errno */
int
tcmur_open(const char * devname, int openflags)
{
    int minor = minor_of_devname(devname);
    //XXXXX need to hold the device
    return minor;
}

error_t
tcmur_close(int minor)
{
    //XXXXX need to unhold the device
    return 0;
}

/* Add a block device of the given minor number.
 * cfg starts with subtype followed by the config string for the underlying
 * tcmu-runner handler:
 *			    /subtype/handler_cfg
 */
error_t
tcmur_device_add(int minor, const char * devname, const char * cfg)
{
    struct tcmu_device * dev;
    size_t size;
    error_t err;

    if (minor >= (int)ARRAY_SIZE(the_tcmu_devices))
	return -ENODEV;

    if (device_of_minor(minor))
	return -EBUSY;

    /* Check that cfg refers to a loaded handler */
    err = tcmur_check_config(cfg);
    if (err)
	return err;

    //XXX Do we need to check for a duplicate cfgstring and -EEXIST?
    //    I think the handler has to do it, because only the handler
    //    knows how to tell if two cfgstrings refer to the same device.
    //	  (But we could still check for identical *strings* here)

    dev = vzalloc(sizeof(*dev));
    dev->rhandler = handler_of_cfgstr(cfg);
    assert(dev->rhandler);

    /* Advance over handler_name to the handler-specific cfg string */
    cfg = strchrnul(cfg+1, '/');

    if (devname) {
	//XXX should make sure name is unique
    	snprintf(dev->dev_name, sizeof(dev->dev_name), "%s", devname);
    } else {
    	snprintf(dev->dev_name, sizeof(dev->dev_name), "%s%03u",
					    dev->rhandler->subtype, minor);
    }
    memset(dev->cfgstring_orig, 0, sizeof(dev->cfgstring_orig));
    strncpy(dev->cfgstring_orig, cfg, sizeof(dev->cfgstring_orig)-1);
    memcpy(dev->cfgstring, dev->cfgstring_orig, sizeof(dev->cfgstring));

    if (dev->rhandler->open) {
	err = dev->rhandler->open(dev, 0);	    /* Call into handler */
	if (err) {
	    tcmu_dev_err(dev, "%s handler->open(%s) returned err=%d\n",
			  dev->rhandler->name, dev->dev_name, err);
	    goto fail_free;
	}
    }

    /* handler->open() might corrupt the config string using strtok() */
    memcpy(dev->cfgstring, dev->cfgstring_orig, sizeof(dev->cfgstring));

#if 0	//XXXXX no tcmu_cfgfs_dev_get_*()
	block_size = tcmu_cfgfs_dev_get_attr_int(dev, "hw_block_size");
	if (block_size <= 0) {
		tcmu_dev_err(dev, "Could not get hw_block_size\n");
		return -EINVAL;
	}

	dev_size = tcmu_cfgfs_dev_get_info_u64(dev, "Size", &ret);
	if (ret < 0) {
		tcmu_dev_err(dev, "Could not get device size\n");
		return -EINVAL;
	}

	max_sectors = tcmu_cfgfs_dev_get_attr_int(dev, "hw_max_sectors");
	if (max_sectors < 0) {
		tcmu_dev_err(dev, "Bad hw_max_sectors (max_xfer_len) %ld\n",
				    max_sectors);
		return -EINVAL;
	}
#else	//XXX

    if (!tcmu_dev_get_block_size(dev)) {
	pr_notice("Using default block size=%d\n", 4096);
	tcmu_dev_set_block_size(dev, 4096);		//XXX 4 KiB
    }

    if (!tcmu_dev_get_num_lbas(dev)) {
	pr_notice("Using default nblocks=%d\n", 262144);
	tcmu_dev_set_num_lbas(dev, 262144);		//XXX 1 GiB
    }

    if (!tcmu_dev_get_max_xfer_len(dev)) {
	pr_notice("Using max I/O size=%d\n", 1024*1024);
	tcmu_dev_set_max_xfer_len(dev, 1024*1024);	//XXX 1 MiB
    }

#endif

    size = dev->num_lbas * dev->block_size;

    tcmu_info("Handler %s attach target %s size %"PRIu64" block_size %d\n",
	       dev->rhandler->name, dev->dev_name, size, dev->block_size);

    the_tcmu_devices[minor] = dev;

    return 0;

fail_free:
    vfree(dev);
    return err;
}

error_t
tcmur_device_remove(int minor)
{
    struct tcmu_device * dev = device_of_minor(minor);
    if (!dev)
	return -ENODEV;

    //XXXXX need to -EBUSY if any holds

    tcmu_info("handler %s destroy tgt: %s\n",
	       dev->rhandler->name, dev->dev_name);

    the_tcmu_devices[minor] = NULL;

    if (dev->rhandler->close)
	dev->rhandler->close(dev);

    vfree(dev);
    return 0;
}

/* Return 0 if OK, otherwise -errno.
 * The name of a handler file is expected to be:
 *	    concatenate(handler_prefix, subtype, ".so")
 */
error_t
tcmur_handler_load(const char * subtype)
{
    char *error;
    char *path;
    void *handle;
    int (*handler_init)(void);
    int ret;
    int i;
    struct tcmur_handler * h;

    h = tcmur_find_handler(subtype);
    if (h) {
	tcmu_err("%s: Handler %s is already registered\n", subtype, h->subtype);
	return -EEXIST;
    }

    for (i = 0; i < (int)ARRAY_SIZE(the_tcmu_handlers); i++)
	if (!the_tcmu_handlers[i])
	    break;	/* found an empty slot */

    if (i >= (int)ARRAY_SIZE(the_tcmu_handlers)) {
	tcmu_err("Out of handler slots trying to register %s\n", subtype);
	return -ENOSPC;
    }

    path = kasprintf(0, "%s%s.so", handler_prefix, subtype);

    handle = dlopen(path, RTLD_NOW|RTLD_LOCAL);
    if (!handle) {
	tcmu_err("Could not open handler at %s: %s\n", path, dlerror());
	ret = -ENOENT;
	goto out_free;
    }

    dlerror();
    handler_init = dlsym(handle, "handler_init");
    if ((error = dlerror())) {
	tcmu_err("dlsym failure on %s: (%s)\n", path, error);
	ret = -EBADF;
	goto err_close;
    }

    ret = handler_init();
    if (ret) {
	tcmu_err("handler_init failed on path %s\n", path);
	ret = -EIO;
	goto err_close;
    }

out_free:
    vfree(path);
    return ret;

err_close:
    dlclose(handle);
    goto out_free;
}

error_t
tcmur_handler_unload(const char * subtype)
{
    int slot;
    int i;

    struct tcmur_handler * h = tcmur_find_handler_slot(subtype, &slot);
    if (!h) {
	tcmu_err("Handler %s is not registered\n", subtype);
	return -ENOENT;
    }

    for (i = 0; i < (int)ARRAY_SIZE(the_tcmu_devices); i++)
	if (the_tcmu_devices[i] && the_tcmu_devices[i]->rhandler == h) {
	    tcmu_err("Handler %s has existing devices\n", subtype);
	    return -EBUSY;
	}

    //XXX dlclose()
    tcmur_unregister_handler(h);
    return 0;
}

/* Client can specify callbacks for device add/remove events */
error_t
libtcmur_init(const char * hprefix)
{
    if (hprefix)
	handler_prefix = hprefix;
    return 0;
}

error_t
libtcmur_exit(void)
{
    int i;
    for (i = 0; i < (int)ARRAY_SIZE(the_tcmu_handlers); i++) {
	if (the_tcmu_handlers[i])
	    return -EBUSY;
    }
    return 0;
}
