/* ram.c -- ramdisk driver for tcmu-runner
 *
 * Copyright 2017-2019 David A. Butterfield
 * MIT License [SPDX:MIT https://opensource.org/licenses/MIT]
 *
 * This tcmu-runner backstore handler does mmap(2) of a backing file or
 * anonymous memory and simply copies to/from the mmap for Write/Read.
 * Flush does msync(2).  Config string should be the pathname of the
 * backing file, or "/@" for an anonymous mmap.
 *
 * Backing files get msync(2) at close time and persist across sessions.
 * Data in anonymous mmaps is discarded at close time.
 * Data can page to swapspace by default; mlock(2) enabled by config flag.
 *
 * XXX Notes areas in need of attention.
 */
#define _GNU_SOURCE
#include <sys/types.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdbool.h>
#include <unistd.h>
#include <errno.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/syscall.h>
#include <assert.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/mman.h>

#include "tcmu-runner.h"
#include "tcmur_device.h"

#define ROUND_DOWN(v, q)    ((v) / (q) * (q))

#ifndef PAGE_SIZE
#define PAGE_SIZE	    4096
#endif

#define BLOCK_SIZE	    PAGE_SIZE
#define DEFAULT_FILE_SIZE   (1*1024*1024*1024l)

typedef struct tcmu_ram {
	void	      *	ram;
	size_t		size;
	int		fd;	    /* when backing file (not anonymous) */
} * state_t;

/* Return true if the mmap memory should be locked */
static inline bool do_mlock(struct tcmu_device *td)
{
	/* XXX Before changing this to return true, need to add
	   sanity checks for td->size to avoid out-of-memory failures */
	return false;	    //XXX Needs a config switch
}

static int tcmu_ram_read(struct tcmu_device *td, struct tcmulib_cmd *cmd,
	      struct iovec *iov, size_t niov, size_t size, off_t seekpos)
{
	int sts = TCMU_STS_OK;
	state_t s = tcmur_dev_get_private(td);

	if (seekpos >= s->size || seekpos + size > s->size)
	    sts = TCMU_STS_RANGE;
	else
	    tcmu_memcpy_into_iovec(iov, niov, s->ram + seekpos, size);

	cmd->done(td, cmd, sts);
	return TCMU_STS_OK;
}

static int tcmu_ram_write(struct tcmu_device *td, struct tcmulib_cmd *cmd,
	       struct iovec *iov, size_t niov, size_t size, off_t seekpos)
{
	int sts = TCMU_STS_OK;
	state_t s = tcmur_dev_get_private(td);

	if (seekpos >= s->size || seekpos + size > s->size)
	    sts = TCMU_STS_RANGE;
	else
	    tcmu_memcpy_from_iovec(s->ram + seekpos, size, iov, niov);

	cmd->done(td, cmd, sts);
	return TCMU_STS_OK;
}

static int tcmu_ram_flush(struct tcmu_device *td, struct tcmulib_cmd *cmd)
{
	state_t s = tcmur_dev_get_private(td);

	if (msync(s->ram, s->size, MS_SYNC) < 0)
		return TCMU_STS_WR_ERR;

	cmd->done(td, cmd, TCMU_STS_OK);
	return TCMU_STS_OK;
}

static void tcmu_ram_close(struct tcmu_device *td)
{
	state_t s = tcmur_dev_get_private(td);

	if (msync(s->ram, s->size, MS_SYNC) < 0) {
		int err = errno;
		tcmu_dev_warn(td, "%s: close cannot msync (%d -- %s)\n",
			      tcmu_dev_get_cfgstring(td), err, strerror(err));
	}

	tcmur_dev_set_private(td, NULL);

	munmap(s->ram, s->size);
	close(s->fd);
	free(s);
}

static int tcmu_ram_open(struct tcmu_device * td, bool reopen)
{
	char *config;
	bool anon;
	int err, mmap_flags, mmap_fd;
	ssize_t file_size;
	void *ram;
	state_t s;

	//XXX kinda hacky until I figure out how it's supposed to be done
	config = tcmu_dev_get_cfgstring(td);
	if (!config || config[0] != '/' || (config[1] == '@'
						&& config[2] == '\0')) {
		anon = true;
		tcmu_dev_info(td, "No backing file configured -- "
			"anonymous memory will be discarded upon close\n");
	} else {
		anon = false;
		tcmu_dev_dbg(td, "tcmu_ram_open config %s\n", config);
	}

	mmap_flags = MAP_SHARED;
	// mmap_flags |= MAP_HUGETLB;	//XXX probably a big perf win
					//    but needs special setup?

	tcmu_dev_set_block_size(td, BLOCK_SIZE);

	if (anon) {
		mmap_flags |= MAP_ANONYMOUS;
		mmap_fd = -1;
		file_size = 0;		//XXXX get file size from cfgstring
	} else {
		mmap_fd = open(config, O_RDWR|O_CLOEXEC|O_CREAT, 0600);
		if (mmap_fd < 0) {
			err = errno;
			tcmu_dev_err(td, "%s: cannot open (%d -- %s)\n",
					 config, err, strerror(err));
			goto out_fail;
		}
		file_size = ROUND_DOWN(lseek(mmap_fd, 0, SEEK_END),
					tcmu_dev_get_block_size(td));
	}

	if (file_size == 0) {
		file_size = DEFAULT_FILE_SIZE;
		tcmu_dev_warn(td, "%s size unspecified, default size=%ld\n",
				    config, file_size);
	}

	tcmu_dev_set_num_lbas(td, file_size / tcmu_dev_get_block_size(td));
	tcmu_dev_info(td, "%s: size determined as %lu\n", config, file_size);

	if (mmap_fd >= 0) {
		if (ftruncate(mmap_fd, file_size) < 0) {
			err = errno;
			tcmu_dev_warn(td, "%s: ftruncate (%d -- %s)\n",
					  config, err, strerror(err));
		}
		if (fallocate(mmap_fd, 0, 0, file_size) < 0) {
			err = errno;
			tcmu_dev_warn(td, "%s: fallocate (%d -- %s)\n",
					  config, err, strerror(err));
		}
	}

	ram = mmap(NULL, file_size, PROT_READ|PROT_WRITE, mmap_flags, mmap_fd, 0);
	if (ram == MAP_FAILED) {
		err = errno;
		tcmu_dev_err(td, "%s: cannot mmap size=%ld (fd=%d) (%d -- %s)\n",
				 config, file_size, mmap_fd, err, strerror(err));
		goto out_close;
	}

	if (do_mlock(td)) {
		if (mlock2(ram, file_size, MLOCK_ONFAULT) < 0) {
			err = errno;
			tcmu_dev_warn(td, "%s: mlock (%d -- %s)\n", config,
					    err, strerror(err));
		}
	}

	s = calloc(1, sizeof(*s));
	if (!s) {
		err = ENOMEM;
		tcmu_dev_err(td, "%s: cannot allocate state (%d -- %s)\n",
				 config, err, strerror(err));
		goto out_unmap;
	}

	s->ram = ram;
	s->size = file_size;
	s->fd = mmap_fd;
	tcmur_dev_set_private(td, s);
	
	tcmu_dev_dbg(td, "config %s, size %ld\n", config, s->size);
	return 0;

out_unmap:
	munmap(ram, file_size);
out_close:
	close(mmap_fd);
out_fail:
	return -err;
}

static const char tcmu_ram_cfg_desc[] =
	"RAM handler config string is the name of the backing file, "
	"or \"/@/size\" for anonymous memory (non-persistent after close)\n";

struct tcmur_handler tcmu_ram_handler = {
	.name	       = "RAM handler",
	.subtype       = "ram",
	.cfg_desc      = tcmu_ram_cfg_desc,
	.open	       = tcmu_ram_open,
	.close	       = tcmu_ram_close,
	.read	       = tcmu_ram_read,
	.write	       = tcmu_ram_write,
	.flush	       = tcmu_ram_flush,
};

int handler_init(void)
{
	return tcmur_register_handler(&tcmu_ram_handler);
}

/* MIT License  [SPDX:MIT https://opensource.org/licenses/MIT]
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to
 * deal in the Software without restriction, including without limitation the
 * rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
 * sell copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */
