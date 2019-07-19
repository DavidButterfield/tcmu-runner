# hacked-up makefile

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
WFLAGS_WARN += -Wredundant-decls
WFLAGS_WARN += -Wformat=2

WFLAGS_WARN += -Wpointer-arith
WFLAGS_WARN += -Wswitch-default
WFLAGS_WARN += -Wbad-function-cast
WFLAGS_WARN += -Wshadow
WFLAGS_WARN += -Wconversion

# This only warns if -fstrict-aliasing is enabled (e.g. by -O2), in which case the warning is important
WFLAGS_WARN += -Wstrict-aliasing

# WFLAGS_WARN += -Wwrite-strings		# Some of these in fuse_tree.c
WFLAGS_INHIBIT += -Wno-sign-conversion		# 1 in simple_log.c
# WFLAGS_WARN += -Wcast-qual			# 1 in ccan/ccan/list/list.h

### silly
WFLAGS_INHIBIT += -Wno-unused-parameter

CFLAGS += $(WFLAGS_WARN) $(WFLAGS_INHIBIT)

###############################################################################

# XXX  try optimized, and without debug
CFLAGS += -DDEBUG
CFLAGS += -DWANT_MAIN

# CFLAGS += -O3

CFLAGS += -g -I. -I.. -I../ccan -DTCMU_USERMODE
LDFLAGS += -rdynamic

TCMU_UTILS += ../CMakeFiles/tcmu.dir/api.c.o
TCMU_UTILS += simple_log.o
# TCMU_UTILS += ../CMakeFiles/tcmu.dir/libtcmu_time.c.o
# TCMU_UTILS += ../CMakeFiles/tcmu.dir/strlcpy.c.o

FUSED += fuse_tree.o
FUSED += fuse_tcmur.o fuse_tcmur_ctl.o
FUSED += libtcmur.o
FUSED += $(TCMU_UTILS) -lfuse

all:	../version.h ../CMakeFiles/tcmu.dir/api.c.o
	$(MAKE) cscope
	$(MAKE) fuse_tcmur

fuse_tcmur: $(FUSED)
	$(CC) $(CFLAGS) -o $@ $(LDFLAGS) $(FUSED) -lpthread -ldl -lc

simple_log.o: simple_log.c
	$(CC) $(CFLAGS) -DHAVE_TYPEOF -DHAVE_ISBLANK -c -o $@ simple_log.c

../version.h:
	$(MAKE) -C ..

../CMakeFiles/tcmu.dir/api.c.o:
	$(MAKE) -C ..

cscope:
	cscope -b *.h *.c
	ctags *.h *.c

clean:
	rm -f *.o *.a *.so fuse_tcmur cscope.out tags

VALGRIND_OPTS = --leak-check=full --show-leak-kinds=all --num-callers=30 --track-fds=yes --sim-hints=no-nptl-pthread-stackcache --track-origins=yes

valgrind:
	valgrind $(VALGRIND_OPTS) --vgdb=full --vgdb-error=1 ./fuse_tcmur

.PHONY = valgrind clean cscope all
