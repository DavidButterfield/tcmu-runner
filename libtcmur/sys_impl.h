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
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#ifdef USE_UMC

    #include "usermode_lib.h"

#else		/***** !USE_UMC (to the end of this file) *****/

#include <features.h>
#include <inttypes.h>
#include <errno.h>
#include <stdbool.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <syscall.h>
#include <execinfo.h>
#include <valgrind/valgrind.h>
#include <sys/epoll.h>
#include <pthread.h>
#include <stdio.h>	//XXX move this and its parts of backtrace() to .c

#ifndef gettid
#define gettid()			((pid_t)(syscall(SYS_gettid)))
#endif

#define printk(fmtargs...)		_printk(""fmtargs)
#define _printk(fmt, args...) \
	    fprintf(stderr, "%s:%d: "fmt, __func__, __LINE__, ##args)

#define sys_backtrace(fmtargs...) impl_sys_backtrace(""fmtargs)
#define impl_sys_backtrace(fmt, args...) do { \
    if (RUNNING_ON_VALGRIND) { \
	fflush(stderr); \
	VALGRIND_PRINTF_BACKTRACE(fmt, ##args); \
    } else { \
	void *bt[3]; \
	int nframe = backtrace(bt, sizeof(bt) / sizeof((bt)[0])); \
	printk("ERROR: "fmt"\n", ##args); \
	fflush(stderr); \
	backtrace_symbols_fd(bt, nframe, fileno(stderr)); \
    } \
} while (0)

#define panic(fmtargs...) do { sys_backtrace("PANIC: "fmtargs); abort(); } while (0)

#define nlprintk(fmt, args...)		printk(fmt"\n", ##args)
#define pr_err(fmtargs...)		printk("ERROR: "fmtargs)
#define pr_warning(fmtargs...)		printk("WARNING: "fmtargs)
#define pr_notice(fmtargs...)		printk("NOTICE: "fmtargs)
#define pr_info(fmtargs...)		printk("INFO: "fmtargs)

#define WARN_ONCE(cond, fmtargs...) \
do { \
    if (cond) { \
	static bool been_here = false; \
	if (!been_here) { \
	    been_here = true; \
	    pr_warning(fmtargs); \
	} \
    } \
} while (0)

#define kasprintf(gfp, fmt, args...) \
({ \
    char * _ret; \
    int const _rc = asprintf(&_ret, fmt, ##args); \
    if (_rc < 0) \
        _ret = NULL; \
    _ret; \
})

#define UMC_system(cmd) system(cmd)

/* Remove the "const" qualifier from a pointer */
static inline void *
_unconstify(const void * cvp)
{
    union { void * vp; void const * cvp; } p;
    p.cvp = cvp;
    return p.vp;
}

static inline void *
vzalloc(size_t size)
{
    return calloc(1, size);
}

static inline void *
vmalloc(size_t size)
{
    return malloc(size);
}

static inline void
vfree(void * p)
{
    free(p);
}

static inline char *
UMC_string_concat_free(char * prefix, char * suffix)
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

    vfree(prefix);
    vfree(suffix);
    return str;
}

struct mutex {
    pthread_mutex_t	lock;
};

static inline void
mutex_init(struct mutex * l)
{
    pthread_mutex_init(&l->lock, NULL);
}

static inline void
mutex_destroy(struct mutex * l)
{
    pthread_mutex_destroy(&l->lock);
}

static inline bool
mutex_trylock(struct mutex * l)
{
    return pthread_mutex_trylock(&l->lock) == 0;
}

static inline void
mutex_lock(struct mutex * l)
{
    pthread_mutex_lock(&l->lock);
}

static inline void
mutex_unlock(struct mutex * l)
{
    pthread_mutex_unlock(&l->lock);
}

/* Use of this function is inherently racy */
static inline bool
mutex_is_locked(struct mutex * l)
{
    if (!mutex_trylock(l)) {
        return true;   /* we couldn't get the mutex, therefore it is locked */
    }
    mutex_unlock(l);       /* unlock the mutex we just locked to test it */
    return false;       /* We got the mutex, therefore it was not locked */
}

struct completion {
    bool volatile	done;
    struct mutex		lock;
    pthread_cond_t	cond;
};

static inline void
init_completion(struct completion * c)
{
    memset(c, 0, sizeof(*c));
    mutex_init(&c->lock);
    pthread_cond_init(&c->cond, NULL);
}

static inline void
complete(struct completion * c)
{
    mutex_lock(&c->lock);
    c->done = true;
    pthread_cond_broadcast(&c->cond);
    mutex_unlock(&c->lock);
}

static inline void
wait_for_completion(struct completion * c)
{
    mutex_lock(&c->lock);
    while (!c->done)
       pthread_cond_wait(&c->cond, &c->lock.lock);
    mutex_unlock(&c->lock);
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
    struct mutex		i_mutex;
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
    mutex_init(&inode->i_mutex);
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

    mutex_destroy(&inode->i_mutex);

    if (inode->UMC_destructor)
	inode->UMC_destructor(inode);
    else
	vfree(inode);
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
