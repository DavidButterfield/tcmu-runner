/* libtcmur.h -- usermode API to tcmu-runner block storage handlers
 *
 * Copyright 2019 David A. Butterfield
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
 * libtcmur makes use of the handler read, write, and flush block I/O functions
 * only -- no calls are made to handle_cmd().
 */
#ifndef LIBTCMUR_H
#define LIBTCMUR_H
#include <errno.h>

#define __struct_tm_defined	/* inhibit struct_tm.h */
#include "sys_impl.h"

/* Include tcmu-runner.h without some things it includes */
#define __TCMU_SCSI_DEFS	/* inhibit scsi_defs.h */
#define __TCMU_ALUA_H		/* inhibit alua.h */
#define __TCMU_SCSI_H		/* inhibit scsi.h */
#undef max
#undef min
#undef ARRAY_SIZE
#define HAVE_ISBLANK		1
#define HAVE_TYPEOF		1
#ifdef USE_UMC
  #define list_node list_head
  /* UMC provides these things */
  #define _SYS_UIO_H
  #define CCAN_LIST_H
#endif
#define ffs UNUSED_ffs
  #include "../tcmu-runner.h"	/* include after sys_impl.h */
#undef ffs

#include "../version.h"

#define MAX_TCMUR_HANDLERS  64	/* concurrently loaded */
#define MAX_TCMUR_MINORS    256	/* all tcmur handlers combined */

typedef int tcmur_status_t;	/* e.g. TCMU_STS_OK -- see libtcmu_common.h */

/* Functions returning type error_t return zero for success, otherwise -errno */

/* Call libtcmur_init() before using libtcmur services.
 * Default handler_prefix if arg is NULL: "/usr/local/lib/tcmu-runner/handler_"
 * Expected handler name concatenates:  handler_prefix  subtype  ".so"
 */
extern error_t libtcmur_init(const char * handler_prefix);
extern error_t libtcmur_exit(void);

/* tcmur_handler_load() will dlopen() a tcmu-runner handler of the given subtype
 * and load it into the program for use.  tcmur_handler_unload() unloads it.
 */
extern error_t tcmur_handler_load(const char * subtype);
extern error_t tcmur_handler_unload(const char * subtype);

/* tcmur_check_config() checks a handler device configuration string for
 * validity.  The handler for the configuration must already have been loaded
 * using tcmur_handler_load().  cfgstring takes this form, specifying the
 * handler's TCMU subtype:
 *				/subtype/handler-cfg-string
 *
 * See tcmu-runner(8) for more about subtype and handler-cfg-string.
 */
extern error_t tcmur_check_config(const char * cfgstring);

/* tcmur_device_add() adds a device, with specified cfgstring, as the specified
 * tcmur minor number.  tcmur_device_remove() removes the specified minor.  The
 * handler is determined from the subtype in the first segment of cfgstring.
 * All tcmur subtypes share a common space of minor numbers.
 */
extern error_t tcmur_device_add(int minor, const char * devname, const char * cfgstring);
extern error_t tcmur_device_remove(int minor);

/* Lookup and hold the device -- returns a minor number or -errno */
extern int tcmur_open(const char * devname, int openflags);
extern error_t tcmur_close(int minor);

#ifndef USE_UMC
/* If we don't have workqueues, just do the I/O directly */
struct workqueue_struct;
struct work_struct {
    void (*fn)(struct work_struct *);
};
#define INIT_WORK(entry, func)	((entry)->fn = (func))
#define queue_work(q, entry)	((entry)->fn(entry))	/* direct call */
#endif

/* Argument to read/write/flush: only tcmur_cmd->done needs to be filled in by
 * the caller; other fields are used internally by the library
 */
struct libtcmur_task {
    struct tcmur_cmd		tcmur_cmd;
    struct work_struct		work_entry;
    struct tcmu_device	      *	dev;
    size_t			nbyte;
    off_t			seekpos;
    uint64_t			t_start;
};

/* tcmur_read(), tcmur_write() and tcmur_flush() start I/O operations to the
 * specified minor.  Errors in the I/O start process can be reported by -errno
 * return from these calls.  A return value of zero denotes a successful I/O
 * start, in which case there will be a completion call to cmd->done(), which
 * may report either an "sts" error, or success (denoted by TCMU_STS_OK).
 *
 * Note that the completion call may occur before the request call returns.
 */
extern error_t tcmur_read(int minor, struct libtcmur_task *,
				struct iovec *, size_t niov, size_t, off_t);
extern error_t tcmur_write(int minor, struct libtcmur_task *,
				struct iovec *, size_t niov, size_t, off_t);
extern error_t tcmur_flush(int minor, struct libtcmur_task *);

/* tcmur_get_dev_name() returns the device name of the specified minor.
 * If the minor does not exist then the return is NULL.
 */
extern const char * tcmur_get_dev_name(int minor);

/* tcmur_get_size() returns the size in bytes of the specified minor.
 * If the minor does not exist then the return is a -errno.
 */
extern ssize_t tcmur_get_size(int minor);
extern ssize_t tcmur_get_block_size(int minor);

/* tcmur_get_max_xfer() returns the maximum I/O size in bytes of the specified
 * minor.  If the minor does not exist then the return is a -errno.
 */
extern ssize_t tcmur_get_max_xfer(int minor);

/* bio_tcmur */
extern error_t bio_tcmur_init(int major, int max_minor);
extern error_t bio_tcmur_exit(void);
extern error_t bio_tcmur_add(int minor);
extern error_t bio_tcmur_remove(int minor);

struct block_device;
extern struct block_device * bio_tcmur_bdev(int minor);

#endif /* LIBTCMUR_H */
