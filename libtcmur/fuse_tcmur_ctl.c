/* fuse_tcmur_ctl.c --
 * translate command strings written to fuse node into libtcmur control actions
 *
 * Copyright 2019 David A. Butterfield
 *
 * This file is licensed to you under your choice of the GNU Lesser
 * General Public License, version 2.1 or any later version (LGPLv2.1 or
 * later), or the Apache License 2.0.
 *
 * Most of the code here implements the command-line interpreter for the tcmur
 * control device, which facilitates loading handlers and adding devices
 * through writes to a control node in the fuse FS.
 *
 * Call fuse_tcmur_ctl_pre() to set up the fuse tree with a few directories and
 * add the tcmur control device.
 */
#define _GNU_SOURCE
#include <sys/types.h>
#include <inttypes.h>
#include <stdbool.h>
#include <errno.h>
#include <limits.h>
#include <stddef.h>
#include <unistd.h>
#include <stdlib.h>
#include <signal.h>
#include <fcntl.h>
#include <ctype.h>
#include <sys/stat.h>

#include "sys_impl.h"
#include "libtcmur.h"
#include "fuse_tree.h"
#include "fuse_tcmur.h"

#include <string.h>	    /* include after sys_impl.h */

/* Interactive printf for responses to commands written to control node */
#define iprintf(fmtargs...) (fprintf(stderr, fmtargs), fflush(stderr))

static struct file_operations * fuse_tcmur_dev_fops;  /* ops for added devices */
static fuse_node_t fnode_dev;	/* /fuse/dev */
static fuse_node_t fnode_mod;	/* /fuse/sys/module */

static void
ctl_help(void)
{
    iprintf(
	"Commands:\n"
	"   add    tcmur_minor_number /subtype/handler_cfgstring\n"
	"   remove tcmur_minor_number\n"
	"   load   handler_subtype\n"
	"   unload handler_subtype\n"
	"   source filename	    # read commands from filename\n"
	"   dump		    # print a representation of the fuse tree\n"
//	"   exit\n"
//	"   echo rest of line up to comment\n"
//	"   # ignore to end of line\n"
//	"   help\n"
    );
}

#define ISblank(c)  isblank((unsigned char)(c))
#define ISprint(c)  isprint((unsigned char)(c))
#define ISalnum(c)  isalnum((unsigned char)(c))

/* Delay sending the shutdown signal so fuse can close the ctldev it received exit command on */
static void
exit_handler(int signum)
{
    kill(getpid(), SIGTERM);
}

/* Returns a freeable copy of the content of the next line of the buffer --
 * The line is trimmed of starting and ending blanks, and ending comments.
 */
static char *
copyline(const char * buf, size_t size)
{
    char * str;	    /* pointer to copy to return */
    const char * q; /* last non-blank seen */
    const char * p;
    int ret;

    /* Advance over initial spaces/tabs */
    while (size && ISblank(*buf))
	buf++, size--;

    /* buf points to the first nonblank on the line */
    q = p = buf;

    /* Find the first '#' or non-printable (e.g. '\n', '\r', '\0') */
    while (size && ISprint(*p) && *p != '#') {
	p++, size--;
	if (!ISblank(*(p-1)))
	    q = p;	    /* track last nonblank seen so far */
    }

    /* Return string starts and ends with non-blank characters */
    ret = sys_asprintf(&str, "%.*s", (int)(q-buf), buf);
    verify_ge(ret, 0, "asprintf: %s", strerror(errno));

    return str;
}

/* Return true if str matches pattern.
 * pattern should be the (alphanumeric) command name in lower case.
 * Matches if str is a (non-empty) initial substring of pattern, ignoring case.
 */
static bool
str_match(const char * str, const char * pattern)
{
    const char * p = str;
    const char * q = pattern;

    if (!ISalnum(*p))
	return false;	    /* require at least one character */

    while (ISalnum(*p))
	if (tolower(*p++) != *q++)
	    return false;

    return true;
}

/* Return a pointer to the next nonblank field, or to the NUL if none --
 * Because we are looking at the copy of one line from the write buffer,
 * we can rely on it being NUL-terminated.
 */
static const char *
nextfield(const char * str)
{
    const char * p = str;

    /* Skip over the rest of this field */
    while (*p && !ISblank(*p))
	p++;

    /* Skip over spaces/tabs to the next field */
    while (ISblank(*p))
	p++;

    return p;
}

#define MAX_SOURCE 4096

/* Interpret a string written to ctldev as a program command */
static ssize_t
ctl_write(struct file * unused, const char * buf, size_t iosize, off_t * lofsp)
{
    const char * cmd_str;
    const char * arg_str;
    const char * line = buf;
    size_t size = iosize;

    while (size) {
	char * copy = copyline(line, size);
	if (*copy)
	    iprintf("> %s\n", copy);	/* echo the command line */

	cmd_str = copy;
	arg_str = nextfield(cmd_str);

	if (str_match(cmd_str, "help")) {
	    ctl_help();
	}

	/* add minor_number /subtype/handler_cfg */
	else if (str_match(cmd_str, "add")) {
	    char * endptr;
	    errno = 0;
	    unsigned long ul = strtoul(arg_str, &endptr, 0);
	    if (errno)
		iprintf("%s: %s\n", strerror(errno), arg_str);
	    else if (*endptr && !ISblank(*endptr))
		iprintf("Bad number: %s\n", arg_str);
	    else if (ul > MAX_TCMUR_MINORS)
		iprintf("Number too big: %ld > %d=max\n",
				    ul, MAX_TCMUR_MINORS-1);
	    else {
		int minor = (int)ul;
		arg_str = nextfield(arg_str);
		if (*arg_str != '/')
		    iprintf("Usage: "
			"add tcmu_minor_number /subtype/handler_cfgstring\n");
		else {
		    error_t err = tcmur_device_add(minor, arg_str);
		    if (err)
			iprintf("tcmur_device_add(%d, \"%s\") returns %d\n",
				minor, arg_str, err);
		    else {
#ifdef CONFIG_BIO
			bio_tcmur_add(minor);
#else
			/* Create the fuse node for the device */
			fuse_node_t fnode = fuse_node_add(
						tcmur_get_dev_name(minor),
						fnode_dev, S_IFBLK|0664,
						fuse_tcmur_dev_fops,
						(void *)(uintptr_t)minor);
			if (fnode) {
			    fuse_node_update_size(fnode,
					(size_t)tcmur_get_size(minor));
			    fuse_node_update_block_size(fnode,
					(size_t)tcmur_get_block_size(minor));
			}
#endif
		    }
		}
	    }
	}

	/* remove minor_number */
	else if (str_match(cmd_str, "remove")) {
	    char * endptr;
	    errno = 0;
	    unsigned long ul = strtoul(arg_str, &endptr, 0);
	    if (errno)
		iprintf("%s: %s\n", strerror(errno), arg_str);
	    else if (*endptr && !ISblank(*endptr))
		iprintf("Bad number: %s\n", arg_str);
	    else if (ul > MAX_TCMUR_MINORS)
		iprintf("Number too big: %ld > %d=max\n", ul, MAX_TCMUR_MINORS-1);
	    else {
		error_t err;
		int minor = (int)ul;
#ifdef CONFIG_BIO
		err = bio_tcmur_remove(minor);
#else
		err = fuse_node_remove(tcmur_get_dev_name(minor), fnode_dev);
		if (err) {
		    iprintf("remove %s (%d): %s\n",
			    tcmur_get_dev_name(minor), minor, strerror(-err));
		}
#endif
		if (!err)
		    tcmur_device_remove(minor);
	    }
	}

	/* load subtype */
	else if (str_match(cmd_str, "load")) {
	    if (!ISalnum(*arg_str))
		iprintf("Usage: load handler_subtype\n");
	    else if (tcmur_handler_load(arg_str) == 0)
		fuse_tree_mkdir(arg_str, fnode_mod);
	}

	/* unload subtype */
	else if (str_match(cmd_str, "unload")) {
	    error_t err;
	    if (!ISalnum(*arg_str))
		iprintf("Usage: unload handler_subtype\n");
	    else if ((err=tcmur_handler_unload(arg_str)) == 0)
		fuse_tree_rmdir(arg_str, fnode_mod);
	    else
		iprintf("%s: %s\n", arg_str, strerror(-err));
	}

	/* source command_filename */
	else if (str_match(cmd_str, "source")) {
	    struct stat statbuf;
	    if (stat(arg_str, &statbuf) < 0) {
		iprintf("%s: %s\n", strerror(errno), arg_str);
		if (*arg_str != '/')
		    iprintf("(Note relative pathnames "
			    "are relative to the server's CWD)\n");
	    } else {
		if (statbuf.st_size > MAX_SOURCE)
		    iprintf("%s too large %lu "
			    "(but you can nest them with 'source')\n",
			    arg_str, statbuf.st_size);
		else {
		    char buffer[statbuf.st_size];
		    int fd = open(arg_str, O_RDONLY);
		    if (fd < 0) {
			iprintf("%s: %s\n", strerror(errno), arg_str);
		    } else {
			ssize_t nbytes = read(fd, buffer, (size_t)statbuf.st_size);
			close(fd);
			if (nbytes > 0)
			    ctl_write(0, buffer, (size_t)nbytes, 0);
		    }
		}
	    }
	}

	else if (str_match(cmd_str, "exit")) {
	    struct sigaction sa = { .sa_handler = exit_handler };
	    sigaction(SIGALRM, &sa, NULL);
	    alarm(1);
	}

	else if (str_match(cmd_str, "echo")) {
	    /* line already echoed */
	}

	else if (str_match(cmd_str, "dump")) {
	    char * str = fuse_tree_fmt();
	    iprintf("%s", str);
	    sys_mem_free(str);
	}

	else if (*cmd_str == '\0') {
	    /* empty line */
	} 

	else {
	    iprintf("  ? %s\nTry 'help'\n", copy);
	}

	sys_mem_free(copy);

	/* Advance the main buffer to the start of the next line */
	while (size && *line && *line != '\n')
	    line++, size--;
	if (size && *line == '\n')
	    line++, size--;
    }

    return (ssize_t)iosize;
}

/* Respond to reads from ctldev with a dump of the fuse tree --
 * *lofsp denotes the starting read position in the dump string.
 */
static ssize_t
ctl_read(struct file * unused, void * buf, size_t iosize, off_t * lofsp)
{
    char * str = fuse_tree_fmt();
    ssize_t ret =  (ssize_t)strlen(str);
    if (ret <= *lofsp)
	return 0;

    ret -= *lofsp;

    strncpy(buf, str + *lofsp, iosize);

    sys_mem_free(str);
    return ret;
}

static struct file_operations ctl_fops = {
    .read = ctl_read,
    .write = ctl_write,
};

error_t
fuse_tcmur_ctl_init(struct file_operations * fops)
{
    assert_ne(fops, NULL);
    assert_eq(fuse_tcmur_dev_fops, NULL);   /* double init */

    /* Thence go ops written to tcmur minors we "add" later */
    fuse_tcmur_dev_fops = fops;

    fnode_dev = fuse_node_lookup("/dev");
    fnode_mod = fuse_node_lookup("/sys/module");

    assert_ne(fnode_dev, 0, "%s", fuse_tree_fmt());
    assert_ne(fnode_mod, 0, "%s", fuse_tree_fmt());

    /* Make the tcmur control node to receive FS writes of commands */
    fuse_tree_mkdir("tcmur", fnode_mod);
    fuse_node_add("tcmur", fnode_dev, 0664, &ctl_fops, 0);

    return 0;
}

error_t
fuse_tcmur_ctl_exit(void)
{
    error_t err;
    assert_ne(fuse_tcmur_dev_fops, 0, "exit without init");

    err = fuse_node_remove("tcmur", fnode_dev);
    if (err)
	return err;
    fnode_dev = NULL;

    fuse_tree_rmdir("tcmur", fnode_mod);
    if (err)
	return err;
    fnode_mod = NULL;

    return 0;
}
