/* fuse_tcmur.h -- translate fuse_node_ops into calls to tcmu-runner handlers
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
#ifndef FUSE_TCMUR_H
#define FUSE_TCMUR_H
#include <errno.h>

/* Initialize the interface from fuse_tree to tcmu-runner handler I/O calls */
extern error_t fuse_tcmur_init(int major, int max_minors);
extern error_t fuse_tcmur_exit(void);

/* fuse_tcmur_ctl_init() sets up a fuse tree with a few initial directories;
 * then adds the tcmur control device, which allows libtcmur to be controlled
 * through commands written into a fuse filesystem node.
 *
 * The argument is the fuse_node_ops vector for tcmur devices subsequently
 * created using the "add" command.
 */
struct fuse_node_ops;
extern error_t fuse_tcmur_ctl_init(struct fuse_node_ops *);
extern error_t fuse_tcmur_ctl_exit(void);

#endif /* FUSE_TCMUR_H */
