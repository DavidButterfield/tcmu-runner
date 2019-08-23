/* sys_assert.h -- assertion macros
 *
 * Copyright 2019 David A. Butterfield
 *
 * This file is licensed to you under your choice of the GNU Lesser
 * General Public License, version 2.1 or any later version (LGPLv2.1 or
 * later), or the Apache License 2.0.
 *
 * assert() family is fatal when DEBUG defined, ignored otherwise
 * expect() family issues warning when DEBUG defined, ignored otherwise
 * verify() family is checked and fatal in all builds
 * assert_static() is checked at compile-time
 *
 * Define sys_backtrace(fmt, args...) before including this file
 */
#ifndef SYS_ASSERT_H
#define SYS_ASSERT_H

#ifndef sys_backtrace
#error #define sys_backtrace(fmt, args...) before including sys_assert.h
#endif

#define assert_static(e) ;enum { _CONCAT(static_assert_, __COUNTER__) = 1/(!!(e)) }

#define _assfail()  abort()

// Avoid expect(x) because DRBD has its own version; use expect_ne(x, 0) instead
#ifdef DEBUG
#define expect_rel(x, op, y, fmtargs...)    _expect_rel((x), op, (y), ""fmtargs)
#define expect_imply(x, y, fmtargs...)	    _expect_imply((x), (y), ""fmtargs)
#define expect_noerr(err, fmtargs...)	    _expect_noerr((err), ""fmtargs)
#define expect_rc(rc, call, fmtargs...)	    _expect_rc((rc), call, ""fmtargs)
#else
#define expect_rel(x, op, y, fmtargs...)    ( _USE(x), _USE(y) )
#define expect_imply(x, y, fmtargs...)	    ( _USE(x), _USE(y) )
#define expect_noerr(err, fmtargs...)	    _USE(err)
#define expect_rc(rc, call, fmtargs...)	    _USE(rc)
#endif

/* Enabled when -DDEBUG:  assert*(), expect*(), verify*(), _expect*() */

#define expect_eq(x, y, fmtargs...)	expect_rel((x), ==, (y), ""fmtargs)
#define expect_ne(x, y, fmtargs...)	expect_rel((x), !=, (y), ""fmtargs)
#define expect_lt(x, y, fmtargs...)	expect_rel((x), <,  (y), ""fmtargs)
#define expect_le(x, y, fmtargs...)	expect_rel((x), <=, (y), ""fmtargs)
#define expect_gt(x, y, fmtargs...)	expect_rel((x), >,  (y), ""fmtargs)
#define expect_ge(x, y, fmtargs...)	expect_rel((x), >=, (y), ""fmtargs)

#undef assert

#define assert(cond, fmtargs...)	(assert_ne((cond), 0, ##fmtargs))
#define assert_eq(x, y, fmtargs...)	(expect_eq((x), (y), ##fmtargs) ?: _assfail())
#define assert_ne(x, y, fmtargs...)	(expect_ne((x), (y), ##fmtargs) ?: _assfail())
#define assert_lt(x, y, fmtargs...)	(expect_lt((x), (y), ##fmtargs) ?: _assfail())
#define assert_le(x, y, fmtargs...)	(expect_le((x), (y), ##fmtargs) ?: _assfail())
#define assert_gt(x, y, fmtargs...)	(expect_gt((x), (y), ##fmtargs) ?: _assfail())
#define assert_ge(x, y, fmtargs...)	(expect_ge((x), (y), ##fmtargs) ?: _assfail())
#define assert_imply(x, y, fmtargs...)	(expect_imply((x), (y), ##fmtargs) ?: _assfail())

/* Always enabled:  verify*(), _expect*() */

#define verify(cond, fmtargs...)	(verify_ne((cond), 0, ##fmtargs))
#define verify_eq(x, y, fmtargs...)	(_expect_eq((x), (y), ##fmtargs) ?: _assfail())
#define verify_ne(x, y, fmtargs...)	(_expect_ne((x), (y), ##fmtargs) ?: _assfail())
#define verify_lt(x, y, fmtargs...)	(_expect_lt((x), (y), ##fmtargs) ?: _assfail())
#define verify_le(x, y, fmtargs...)	(_expect_le((x), (y), ##fmtargs) ?: _assfail())
#define verify_gt(x, y, fmtargs...)	(_expect_gt((x), (y), ##fmtargs) ?: _assfail())
#define verify_ge(x, y, fmtargs...)	(_expect_ge((x), (y), ##fmtargs) ?: _assfail())
#define verify_imply(x, y, fmtargs...)	(_expect_imply((x), (y), ##fmtargs) ?: _assfail())

#define _expect(cond, fmt, args...) ({ \
    intptr_t _c = (intptr_t)(cond);   /* evaluate cond exactly once */ \
    if (!(_c)) \
	sys_backtrace("CONDITION FAILED: %s\n"fmt, #cond, ##args); \
    _c;	/* return the full value of cond */ \
})

#define _expect_rel(xx, rel, yy, fmt, args...) ({ \
    typeof(xx) x = (xx); \
    typeof(yy) y = (yy); \
    _expect((intptr_t)x rel (intptr_t)y, \
	    "%s %ld (0x%lx) SHOULD BE %s (0x%lx) %ld %s "fmt, \
	    #xx, (intptr_t)(x), (intptr_t)(x), #rel, \
		 (intptr_t)(y), (intptr_t)(y), #yy, ##args); \
})

#define _expect_eq(x, y, fmtargs...)	_expect_rel((x), ==, (y), ""fmtargs)
#define _expect_ne(x, y, fmtargs...)	_expect_rel((x), !=, (y), ""fmtargs)
#define _expect_lt(x, y, fmtargs...)	_expect_rel((x), <,  (y), ""fmtargs)
#define _expect_le(x, y, fmtargs...)	_expect_rel((x), <=, (y), ""fmtargs)
#define _expect_gt(x, y, fmtargs...)	_expect_rel((x), >,  (y), ""fmtargs)
#define _expect_ge(x, y, fmtargs...)	_expect_rel((x), >=, (y), ""fmtargs)

#define _expect_imply(xx, yy, fmt, args...) ({ \
    typeof(xx) x = (xx); \
    typeof(yy) y = (yy); \
    _expect(!(intptr_t)(x) || !!(intptr_t)(y), \
	    "%s %ld (%lx) SHOULD IMPLY (%lx) %ld %s"fmt, \
	    #xx, (intptr_t)(x), (intptr_t)(x), \
		 (intptr_t)(y), (intptr_t)(y), #yy, ##args); \
})

/* Expects (err == 0) -- works on kernel-style errnos and userland-style errnos */
#define _expect_noerr(err, fmt, args...) \
    _expect_eq((err), 0, "syscall error "fmt": errno=%d %s", \
			    ##args, (int)err, strerror(err>0?err:-err))

/* Expects (rc >= 0) -- works on kernel-style errnos and userland-style errnos */
#define _expect_rc(rc, call, fmt, args...)					\
    _expect_ge((rc), 0, "%s syscall: rc=%d err=%d %s "fmt,			\
		    #call, (int)rc, (rc) == -1 ? errno : (int)-(rc),		\
			   strerror((rc) == -1 ? errno : (int)-(rc)), ##args)

#define verify_noerr(err, fmtargs...)	(_expect_noerr((err), ""fmtargs) ?: _assfail())
#define verify_rc(rc, call, fmtargs...)	(_expect_rc((rc), call, ""fmtargs) ?: _assfail())

#endif /* SYS_ASSERT_H */
