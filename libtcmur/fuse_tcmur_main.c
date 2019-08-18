/* fuse_tcmur_main.c -- interface from fuse to tcmu-runner handlers
 *
 * Copyright 2019 David A. Butterfield
 *
 * This file is licensed to you under your choice of the GNU Lesser
 * General Public License, version 2.1 or any later version (LGPLv2.1 or
 * later), or the Apache License 2.0.
 *
 * Main program uses fuse_tree and libtcmur to provide mountable access to
 * tcmu-runner devices.
 */
#define _GNU_SOURCE
#include <inttypes.h>
#include <stdbool.h>
#include <errno.h>

#include "sys_impl.h"
#include "libtcmur.h"
#include "fuse_tree.h"
#include "fuse_tcmur.h"

/* For failure (non-zero), prefers a -errno from FN */
#define DO_OR_DIE(FN) \
do { \
    error_t _err = (FN); \
    if (_err) { \
	sys_error("'%s' err=%d %s\n", \
		    #FN, _err, _err < 0 ? strerror(-_err) : ""); \
	sys_abort(); \
    } \
} while (0)

#define DO_OR_WARN(FN) \
do { \
    error_t _err = (FN); \
    if (_err) { \
	sys_warning("'%s' err=%d %s\n", \
		    #FN, _err, _err < 0 ? strerror(-_err) : ""); \
    } \
} while (0)

#define DEFAULT_FUSE_TCMUR_MOUNTPOINT "/tcmur"
static const char * mountpoint = DEFAULT_FUSE_TCMUR_MOUNTPOINT;
static const char * handler_prefix = DEFAULT_HANDLER_PATH "/handler_";
static int tcmur_major_number;
static int tcmur_max_minors = 256;
#ifdef CONFIG_BIO
static bool enable_bio = false;
#endif

//XXX Add command-line options to specify:
//	    mountpoint
//	    handler_prefix
//	    enable_bio
//	    tcmur_major_number
//	    tcmur_max_minors

int main(int argc, char * argv[])
{
    fuse_node_t fnode_sys;

    DO_OR_DIE(libtcmur_init(handler_prefix));	/* prepare libtcmur */

    DO_OR_DIE(fuse_tree_init(mountpoint));	/* prepare fuse */

    /* create /dev, /sys/module */
    DO_OR_DIE(!(             fuse_tree_mkdir("dev", NULL) != 0));
    DO_OR_DIE(!((fnode_sys = fuse_tree_mkdir("sys", NULL)) != 0));
    DO_OR_DIE(!(             fuse_tree_mkdir("module", fnode_sys) != 0));

#ifdef CONFIG_BIO
    if (enable_bio) {
	/* init fuse->bio translation and bio->tcmur translation */
	DO_OR_DIE(fuse_bio_init());
	DO_OR_DIE(bio_tcmur_init(tcmur_major_number, tcmur_max_minors));
    } else
#endif
      /* init direct fuse->tcmur translation */
      DO_OR_DIE(fuse_tcmur_init(tcmur_major_number, tcmur_max_minors));

    DO_OR_WARN(fuse_loop_run(NULL));		/* run fuse_main() */

#ifdef CONFIG_BIO
    if (enable_bio) {
	DO_OR_WARN(bio_tcmur_exit());
	DO_OR_WARN(fuse_bio_exit());
    } else
#endif
      DO_OR_WARN(fuse_tcmur_exit());

    /* remove /dev, /sys/module if empty */
    DO_OR_WARN(fuse_tree_rmdir("dev", NULL));
    DO_OR_WARN(fuse_tree_rmdir("module", fnode_sys));
    DO_OR_WARN(fuse_tree_rmdir("sys", NULL));

    /* -EBUSY if non-root nodes still exist */
    DO_OR_WARN(fuse_tree_exit());

    /* -EBUSY if handler(s) still loaded */
    DO_OR_WARN(libtcmur_exit());

    return 0;
}
