# hacked-up makefile with bogus dependencies

#CONFIG_BIO = 1		# define this to use BIO

ifdef CONFIG_BIO

CFLAGS += -DCONFIG_BIO -DUSE_UMC
CFLAGS += -I ../../usermode_compat/src
CFLAGS += -I ../../usermode_compat/src/arch/x86/include
CFLAGS += -I ../../usermode_compat/src/include

# For sys_service.h
CFLAGS += -I ../../MTE/include

endif

CFLAGS += -DDEBUG
# CFLAGS += -O3

###############################################################################

WFLAGS_WARN += -Wall
WFLAGS_WARN += -Wmissing-prototypes
WFLAGS_WARN += -Wstrict-prototypes
WFLAGS_WARN += -Wundef

WFLAGS_WARN += -Wextra
WFLAGS_WARN += -Wjump-misses-init
WFLAGS_WARN += -Wlogical-op
WFLAGS_WARN += -Wcast-align
WFLAGS_WARN += -Wold-style-definition 
WFLAGS_WARN += -Wmissing-include-dirs
WFLAGS_WARN += -Wfloat-conversion
WFLAGS_WARN += -Wmissing-declarations
WFLAGS_WARN += -Wnested-externs
WFLAGS_WARN += -Wunsafe-loop-optimizations

WFLAGS_WARN += -Winit-self
WFLAGS_WARN += -Wswitch-enum
WFLAGS_WARN += -Wformat=2

WFLAGS_WARN += -Wswitch-default
WFLAGS_WARN += -Wbad-function-cast

ifndef CONFIG_BIO

WFLAGS_WARN += -Wconversion
WFLAGS_WARN += -Wpointer-arith
WFLAGS_WARN += -Wredundant-decls
WFLAGS_WARN += -Wshadow

else

#XXX These occur when compiling using UMC
# WFLAGS_WARN += -Wconversion
# WFLAGS_WARN += -Wpointer-arith
# WFLAGS_WARN += -Wredundant-decls
# WFLAGS_WARN += -Wshadow
WFLAGS_INHIBIT += -Wno-sign-compare
WFLAGS_INHIBIT += -Wno-attributes

endif

# This only warns if -fstrict-aliasing is enabled (e.g. by -O2), in which case the warning is important
WFLAGS_WARN += -Wstrict-aliasing

### WFLAGS_WARN += -Wwrite-strings		# Some of these in fuse_tree.c
### WFLAGS_WARN += -Wcast-qual			# 1 in ccan/ccan/list/list.h

# silly
WFLAGS_INHIBIT += -Wno-unused-parameter

CFLAGS += $(WFLAGS_WARN) $(WFLAGS_INHIBIT)

###############################################################################

CFLAGS += $(MY_CFLAGS) $(EXTRA_CFLAGS)

CFLAGS += -g -I. -I.. -I../ccan -DTCMU_USERMODE
LDFLAGS += -rdynamic
LIBS += -lfuse -lpthread -ldl -lc

TCMU_UTILS += ../CMakeFiles/tcmu.dir/api.c.o
# TCMU_UTILS += ../CMakeFiles/tcmu.dir/libtcmu_time.c.o	# optional
# TCMU_UTILS += ../CMakeFiles/tcmu.dir/strlcpy.c.o	# optional

RUNNER += libtcmur.o simple_log.o $(TCMU_UTILS)

FUSE += fuse_tcmur.o fuse_tcmur_ctl.o fuse_tree.o

H_SYS += sys_impl.h sys_assert.h
H_FUSE += fuse_tree.h fuse_tcmur.h
H_RUNNER += libtcmur.h

H_ALL += $(H_SYS) $(H_FUSE) $(H_RUNNER)

.c.o:	$(H_ALL)

ifndef CONFIG_BIO

all:	fuse_tcmur
	$(MAKE) -i cscope

else

all:	libfusetree.a libtcmur.a
	$(MAKE) -i cscope

endif

fuse_tcmur: ../version.h fuse_tcmur_main.o libfusetree.a libtcmur.a
	$(CC) $(CFLAGS) -o $@ $(LDFLAGS) fuse_tcmur_main.o libfusetree.a libtcmur.a $(LIBS)

libtcmur.a: ../version.h $(RUNNER)
	$(LD) -o $@ -r $(RUNNER)

libfusetree.a:	$(FUSE)
	$(LD) -o $@ -r $(FUSE)

simple_log.o: simple_log.c
	$(CC) $(CFLAGS) -DHAVE_TYPEOF -DHAVE_ISBLANK -c -o $@ simple_log.c

$(TCMU_UTILS) ../version.h:
	$(MAKE) -C ..

cscope:
	cscope -b   `find -name \*.c -o -name \*.h`
	ctags	    `find -name \*.c -o -name \*.h`

clean:
	rm -f *.o *.a fuse_tcmur cscope.out tags

extraclean: clean

VALGRIND_OPTS = --leak-check=full --show-leak-kinds=all --num-callers=30 --track-fds=yes --sim-hints=no-nptl-pthread-stackcache --track-origins=yes

valgrind:
	valgrind $(VALGRIND_OPTS) --vgdb=full ./fuse_tcmur

valgdb:
	valgrind $(VALGRIND_OPTS) --vgdb=full --vgdb-error=1 ./fuse_tcmur

.PHONY = valgrind clean cscope all
