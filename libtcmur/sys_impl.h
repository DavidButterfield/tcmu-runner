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

#include <inttypes.h>
#include <errno.h>
#include <stdbool.h>
#include <string.h>	    // XXX strerror, memset, strncpy
#include <stdlib.h>	    // calloc, etc  //XXX
#include <unistd.h>	    // syscall	//XXX
#include <syscall.h>
#include <execinfo.h>
#include <valgrind.h>

#include <stdio.h>	    // fprintf, etc XXXXXX move to .c file
#include <sys/epoll.h>	    // XXX move to .c file
#include <pthread.h>	    // XXX move to .c file?

#define sys_trace(fmtargs...)		sys_msg("TRACE: "fmtargs)
#define sys_error(fmtargs...)		sys_msg("ERROR: "fmtargs)
#define sys_warning(fmtargs...)		sys_msg("WARNING: "fmtargs)
#define sys_notice(fmtargs...)		sys_msg("NOTICE: "fmtargs)
#define sys_info(fmtargs...)		sys_msg("INFO: "fmtargs)

#define sys_msg(fmtargs...)		_sys_msg(""fmtargs)
#define _sys_msg(fmt, args...) \
	    fprintf(stderr, "%s:%d: "fmt"\n", __func__, __LINE__, ##args)

#include "sys_assert.h"		/* include after defining sys_error */

#define gettid()			((pid_t)(syscall(SYS_gettid)))
#define sys_abort()			({ sys_backtrace("abort"); abort(); })

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

//XXXXXXX someone should call me!
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

#endif /* SYS_IMPL_H */
