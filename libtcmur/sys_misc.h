/* sys_misc.h -- common low-level functions
 *
 * Copyright 2019 David A. Butterfield
 *
 * This file is licensed to you under your choice of the GNU Lesser
 * General Public License, version 2.1 or any later version (LGPLv2.1 or
 * later), or the Apache License 2.0.
 *
 * misc compiler tricks
 * sys_error, sys_warning, sys_notice, sys_info
 * expect, assert, verify, sys_backtrace
 * sys_string_concat_free
 * sys_mutex, sys_completion
 */
#ifndef SYS_MISC_H
#define SYS_MISC_H
#include <sys/types.h>
#include <inttypes.h>
#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <execinfo.h>
#include <valgrind.h>
#include <syscall.h>

#define gettid()		((pid_t)(syscall(SYS_gettid)))

#define _USE(x)			({ if (0 && (uintptr_t)(x)==0) {}; 0; })

#define ARRAY_SIZE(arr)		(sizeof(arr) / sizeof((arr)[0]))

#define container_of(member_ptr, outer_type, member) \
    ((outer_type *)((char *)(member_ptr) - offsetof(outer_type, member)))

/* Remove the "const" qualifier from a pointer */
static inline void *
_unconstify(const void * cvp)
{
    union { void * vp; void const * cvp; } p;
    p.cvp = cvp;
    return p.vp;
}

#define sys_error(fmtargs...)	sys_msg("ERROR: "fmtargs)
#define sys_warning(fmtargs...)	sys_msg("WARNING: "fmtargs)
#define sys_notice(fmtargs...)	sys_msg("NOTICE: "fmtargs)
#define sys_info(fmtargs...)	sys_msg("INFO: "fmtargs)

#define sys_msg(fmtargs...)	_sys_msg(""fmtargs)
#define _sys_msg(fmt, args...) \
	    fprintf(stderr, "%s:%d: "fmt"\n", __func__, __LINE__, ##args)

#define sys_backtrace(fmtargs...) _sys_backtrace(""fmtargs)
#define _sys_backtrace(fmt, args...) do { \
    if (RUNNING_ON_VALGRIND) { \
	fflush(stderr); \
	VALGRIND_PRINTF_BACKTRACE(fmt, ##args); \
    } else { \
	void *bt[64]; \
	int nframe = backtrace(bt, ARRAY_SIZE(bt)); \
	sys_error(fmt, ##args); \
	fflush(stderr); \
	backtrace_symbols_fd(bt, nframe, fileno(stderr)); \
    } \
} while (0)

static inline uintptr_t
_assfail(void)
{
    sys_backtrace("ASSERTION FAILED");
    abort();
    return 0;
}

#ifdef DEBUG
#define expect(cond, fmtargs...)	_expect(cond, ""fmtargs)
#define expect_rel(x, op, y, fmtargs...) _expect_rel((x), op, (y), ""fmtargs)
#define expect_imply(x, y, fmtargs...)	_expect_imply((x), (y), ""fmtargs)
#else
#define expect(cond, fmtargs...)	( _USE(cond) )
#define expect_rel(x, op, y, fmtargs...) ( _USE(x), _USE(y) )
#define expect_imply(x, y, fmtargs...)	( _USE(x), _USE(y) )
#endif

/* Enabled when -DDEBUG */

#define expect_eq(x, y, fmtargs...) expect_rel((x), ==, (y), ""fmtargs)
#define expect_lt(x, y, fmtargs...) expect_rel((x), <,  (y), ""fmtargs)
#define expect_le(x, y, fmtargs...) expect_rel((x), <=, (y), ""fmtargs)
#define expect_gt(x, y, fmtargs...) expect_rel((x), >,  (y), ""fmtargs)
#define expect_ge(x, y, fmtargs...) expect_rel((x), >=, (y), ""fmtargs)

#define assert(cond, fmtargs...)	(expect(cond, ##fmtargs) ?: _assfail())
#define assert_eq(x, y, fmtargs...)	(expect_eq((x), (y), ##fmtargs) ?: _assfail())
#define assert_lt(x, y, fmtargs...)	(expect_lt((x), (y), ##fmtargs) ?: _assfail())
#define assert_le(x, y, fmtargs...)	(expect_le((x), (y), ##fmtargs) ?: _assfail())
#define assert_gt(x, y, fmtargs...)	(expect_gt((x), (y), ##fmtargs) ?: _assfail())
#define assert_ge(x, y, fmtargs...)	(expect_ge((x), (y), ##fmtargs) ?: _assfail())
#define assert_imply(x, y, fmtargs...)	(expect_imply((x), (y), ##fmtargs) ?: _assfail())

/* Always enabled */

#define verify(cond, fmtargs...)	(_expect(cond, ##fmtargs) ?: _assfail())
#define verify_eq(x, y, fmtargs...)	(_expect_eq((x), (y), ##fmtargs) ?: _assfail())
#define verify_lt(x, y, fmtargs...)	(_expect_lt((x), (y), ##fmtargs) ?: _assfail())
#define verify_le(x, y, fmtargs...)	(_expect_le((x), (y), ##fmtargs) ?: _assfail())
#define verify_gt(x, y, fmtargs...)	(_expect_gt((x), (y), ##fmtargs) ?: _assfail())
#define verify_ge(x, y, fmtargs...)	(_expect_ge((x), (y), ##fmtargs) ?: _assfail())
#define verify_imply(x, y, fmtargs...)	(_expect_imply((x), (y), ##fmtargs) ?: _assfail())

#define _expect(cond, fmt, args...) ({ \
    uintptr_t c = (uintptr_t)(cond);   /* evaluate cond exactly once */ \
    if (!(c)) \
	sys_backtrace("CONDITION FAILED: %s\n"fmt, #cond, ##args); \
    c;	/* return the full value of cond */ \
})

#define _expect_rel(xx, rel, yy, fmt, args...) ({ \
    uintptr_t x = (uintptr_t)(xx); \
    uintptr_t y = (uintptr_t)(yy); \
    _expect(x rel y, \
	    "%s %ld (%lx) SHOULD BE %s (%lx) %ld %s "fmt, \
	    #xx, x, x, #rel, y, y, #yy, ##args); \
})

#define _expect_eq(x, y, fmtargs...) _expect_rel((x), ==, (y), ""fmtargs)
#define _expect_lt(x, y, fmtargs...) _expect_rel((x), <,  (y), ""fmtargs)
#define _expect_le(x, y, fmtargs...) _expect_rel((x), <=, (y), ""fmtargs)
#define _expect_gt(x, y, fmtargs...) _expect_rel((x), >,  (y), ""fmtargs)
#define _expect_ge(x, y, fmtargs...) _expect_rel((x), >=, (y), ""fmtargs)

#define _expect_imply(x, y, fmt, args...) \
    _expect(!(uintptr_t)(x) || !!(uintptr_t)(y), \
	    "%s %ld (%lx) SHOULD IMPLY (%lx) %ld %s"fmt, \
	    #x, (uintptr_t)(x), (uintptr_t)(x), \
		(uintptr_t)(y), (uintptr_t)(y), #y, ##args)

/******************************************************************************/

/* Put all the memory allocation/free in one place */

static inline void *
sys_zalloc(size_t size)
{
    return calloc(1, size);
}

static inline void
sys_free(void * p)
{
    free(p);
}

static inline char *
sys_string_concat_free(char * prefix, char * suffix)
{
    char * str;
    int ret = asprintf(&str, "%s%s", prefix, suffix);
    verify_ge(ret, 0, "asprintf: %s", strerror(errno));

    free(prefix);
    free(suffix);
    return str;
}

static inline char *
sys_strdup(const char * s)
{
    return strdup(s);
}

#define sys_asprintf(fmtargs...)    asprintf(fmtargs)

/******************************************************************************/

typedef struct sys_mutex {
    pthread_mutex_t	lock;
} sys_mutex_t;

struct sys_completion {
    bool volatile	done;
    sys_mutex_t		lock;
    pthread_cond_t	cond;
};

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
        return true;	/* we couldn't get the mutex, therefore it is locked */
    }
    sys_mutex_unlock(l);	/* unlock the mutex we just locked to test it */
    return false;       /* We got the mutex, therefore it was not locked */
}

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

#endif /* SYS_MISC_H */
