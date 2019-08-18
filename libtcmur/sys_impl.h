/* sys_impl.h -- minimal low-level functions for fuse_tcmur
 *
 * Copyright 2019 David A. Butterfield
 *
 * This file is licensed to you under your choice of the GNU Lesser
 * General Public License, version 2.1 or any later version (LGPLv2.1 or
 * later), or the Apache License 2.0.
 */
#ifndef SYS_IMPL_H
#define SYS_IMPL_H
#define _GNU_SOURCE
#define PATH_MAX 4096

#if defined(USE_UMC)

#include "usermode_lib.h"
#define sys_string_concat_free	UMC_string_concat_free
#define sys_system		system
#define sys_notice		pr_notice
#define sys_warning		pr_warning
#define sys_error		pr_err
#define sys_trace		nlprintk
#define sys_vfprintf		vfprintf
typedef struct mutex		sys_mutex_t;
#define sys_mutex		mutex
#define sys_mutex_init		mutex_init
#define sys_mutex_lock		mutex_lock
#define sys_mutex_unlock	mutex_unlock
#define sys_mutex_is_locked	mutex_is_locked
#define sys_completion		completion
#define sys_complete		complete
#define sys_wait_for_completion	wait_for_completion
#define sys_completion_init	init_completion

#define do_backtrace(fmtargs...) \
do { \
    char * str; \
    sys_asprintf(&str, fmtargs); \
    sys_backtrace(str); \
} while (0)

#else	/* !USE_UMC (to the end of this file) */

#include <features.h>
#include <inttypes.h>
#include <errno.h>
#include <stdbool.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <syscall.h>
#include <execinfo.h>
#include <valgrind.h>

#include <stdio.h>
#include <sys/epoll.h>
#include <pthread.h>

#define sys_trace(fmtargs...)		sys_msg("TRACE: "fmtargs)
#define sys_error(fmtargs...)		sys_msg("ERROR: "fmtargs)
#define sys_warning(fmtargs...)		sys_msg("WARNING: "fmtargs)
#define sys_notice(fmtargs...)		sys_msg("NOTICE: "fmtargs)
#define sys_info(fmtargs...)		sys_msg("INFO: "fmtargs)

#define sys_msg(fmtargs...)		_sys_msg(""fmtargs)
#define _sys_msg(fmt, args...) \
	    fprintf(stderr, "%s:%d: "fmt"\n", __func__, __LINE__, ##args)

#include "sys_assert.h"		/* include after defining sys_error */

#define WARN_ONCE(cond, fmtargs...) \
do { \
    static bool been_here = false; \
    if (!been_here) { \
	been_here = true; \
	sys_warning(fmtargs); \
    } \
} while (0)

#ifndef gettid
#define gettid()			((pid_t)(syscall(SYS_gettid)))
#endif

#define sys_abort()			({ do_backtrace("abort"); abort(); })

#define sys_system(cmd)			system(cmd)
#define sys_vfprintf(fmtargs...)	vfprintf(fmtargs)
#define sys_asprintf(fmtargs...)	asprintf(fmtargs)

/* Remove the "const" qualifier from a pointer */
static inline void *
_unconstify(const void * cvp)
{
    union { void * vp; void const * cvp; } p;
    p.cvp = cvp;
    return p.vp;
}

static inline void *
sys_mem_zalloc(size_t size)
{
    return calloc(1, size);
}

static inline void *
sys_mem_alloc(size_t size)
{
    return malloc(size);
}

static inline void
sys_mem_free(void * p)
{
    free(p);
}

static inline char *
sys_string_concat_free(char * prefix, char * suffix)
{
    char * str;
    int ret;

    if (!suffix)
	return prefix;
    if (!prefix)
	return suffix;

    ret = asprintf(&str, "%s%s", prefix, suffix);
    if (ret < 0)
	str = NULL;

    sys_mem_free(prefix);
    sys_mem_free(suffix);
    return str;
}

typedef struct sys_mutex {
    pthread_mutex_t	lock;
} sys_mutex_t;

static inline void
sys_mutex_init(sys_mutex_t * l)
{
    pthread_mutex_init(&l->lock, NULL);
}

static inline void
sys_mutex_destroy(sys_mutex_t * l)
{
    pthread_mutex_destroy(&l->lock);
}

static inline error_t
sys_mutex_trylock(sys_mutex_t * l)
{
    return -pthread_mutex_trylock(&l->lock);
}

static inline void
sys_mutex_lock(sys_mutex_t * l)
{
    pthread_mutex_lock(&l->lock);
}

static inline void
sys_mutex_unlock(sys_mutex_t * l)
{
    pthread_mutex_unlock(&l->lock);
}

/* Use of this function is inherently racy */
static inline bool
sys_mutex_is_locked(sys_mutex_t * l)
{
    if (!sys_mutex_trylock(l)) {
        return true;   /* we couldn't get the mutex, therefore it is locked */
    }
    sys_mutex_unlock(l);       /* unlock the mutex we just locked to test it */
    return false;       /* We got the mutex, therefore it was not locked */
}

struct sys_completion {
    bool volatile	done;
    sys_mutex_t		lock;
    pthread_cond_t	cond;
};

static inline void
sys_completion_init(struct sys_completion * c)
{
    memset(c, 0, sizeof(*c));
    sys_mutex_init(&c->lock);
    pthread_cond_init(&c->cond, NULL);
}

static inline void
sys_complete(struct sys_completion * c)
{
    sys_mutex_lock(&c->lock);
    c->done = true;
    pthread_cond_broadcast(&c->cond);
    sys_mutex_unlock(&c->lock);
}

static inline void
sys_wait_for_completion(struct sys_completion * c)
{
    sys_mutex_lock(&c->lock);
    while (!c->done)
       pthread_cond_wait(&c->cond, &c->lock.lock);
    sys_mutex_unlock(&c->lock);
}

/**************************************/

#define __barrier() __sync_synchronize()

typedef struct { volatile int counter; } atomic_t;
#define atomic_get(ptr)	({ __barrier(); int ag_ret = (ptr)->counter; __barrier(); ag_ret; })

typedef unsigned int umode_t;
struct fuse_node;

struct inode {
    /* set by init_inode() */
    int				UMC_type;	/* I_TYPE_* */
    int				UMC_fd;		/* backing usermode real fd */
    atomic_t                    i_count;        /* refcount */
    umode_t			i_mode;		/* e.g. S_ISREG */
    off_t			i_size;		/* device or file size in bytes */
    unsigned int		i_flags;	/* O_RDONLY, O_RDWR */
    struct sys_mutex		i_mutex;
    time_t			i_atime;
    time_t			i_mtime;
    time_t			i_ctime;
    struct block_device	      * i_bdev;		/* when I_TYPE_BDEV */
    unsigned int		i_blkbits;	/* log2(block_size) */
    dev_t			i_rdev;		/* device major/minor */
    struct fuse_node	      * pde;            /* when I_TYPE_PROC */
    void			(*UMC_destructor)(struct inode *);
};

#define I_TYPE_PROC			3

static inline void
init_inode(struct inode * inode, int type, umode_t mode,
			size_t size, unsigned int oflags, int fd)
{
    memset(inode, 0, sizeof(*inode));
    inode->UMC_type = type;
    inode->UMC_fd = fd;
    inode->i_count.counter = 1;
    inode->i_mode = mode;
    inode->i_size = (off_t)size;
    inode->i_flags = oflags;
    sys_mutex_init(&inode->i_mutex);
    inode->i_atime = inode->i_mtime = inode->i_ctime = time(NULL);
}

static inline void
_iget(struct inode * inode)
{
    __sync_add_and_fetch(&inode->i_count.counter, 1);
}

static inline void
iput(struct inode * inode)
{
    if (__sync_sub_and_fetch(&inode->i_count.counter, 1))
	return;

    sys_mutex_destroy(&inode->i_mutex);

    if (inode->UMC_destructor)
	inode->UMC_destructor(inode);
    else
	sys_mem_free(inode);
}

struct file {
    void		      * private_data;	/* e.g. seq_file */
    struct inode	      * inode;
};

#define file_inode(file)		((file)->inode)
#define file_pde(file)			(file_inode(file)->pde)
#define file_pde_data(file)		(file_pde(file)->data)

struct file_operations {
    int		 (* open)(struct inode *, struct file *);
    int		 (* release)(struct inode * unused, struct file *);
    ssize_t	 (* write)(struct file *, const char * buf, size_t len, loff_t * ofsp);
    ssize_t	 (* read)(struct file *, void * buf, size_t len, loff_t * ofsp);
    int		 (* fsync)(struct file *, int datasync);
};

#endif	/* !USE_UMC */
#endif /* SYS_IMPL_H */
