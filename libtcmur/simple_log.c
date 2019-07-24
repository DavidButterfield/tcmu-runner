/* simple_log.c -- simplified logging implementation for tcmu-runner handlers
 *
 * Copyright 2016-2017 China Mobile, Inc.
 *
 * This file is licensed to you under your choice of the GNU Lesser
 * General Public License, version 2.1 or any later version (LGPLv2.1 or
 * later), or the Apache License 2.0.
 */
#define _GNU_SOURCE
#include <sys/types.h>
#include <stdint.h>
#include <errno.h>
#include <limits.h>
#include <string.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

#include "libtcmur.h"	    /* must include before sys_impl.h */
#include "sys_impl.h"	    /* must include before libtcmu_config.h */

#include "../libtcmu_log.h"
#include "../libtcmu_config.h"
#include "../libtcmu_time.h"

#include <pthread.h>

static int tcmu_log_level = TCMU_LOG_INFO;

/* Stub out logfile handling functions */
int tcmu_setup_log(char *log_dir) { return 0; }
int tcmu_resetup_log_file(struct tcmu_config *cfg, char *log_dir) { return 0; }
void tcmu_destroy_log(void) { }

/* covert log level from tcmu config to syslog */
static inline int to_syslog_level(int level)
{
	switch (level) {
	case TCMU_CONF_LOG_CRIT:
		return TCMU_LOG_CRIT;
	case TCMU_CONF_LOG_ERROR:
		return TCMU_LOG_ERROR;
	case TCMU_CONF_LOG_WARN:
		return TCMU_LOG_WARN;
	case TCMU_CONF_LOG_INFO:
		return TCMU_LOG_INFO;
	case TCMU_CONF_LOG_DEBUG:
		return TCMU_LOG_DEBUG;
	case TCMU_CONF_LOG_DEBUG_SCSI_CMD:
		return TCMU_LOG_DEBUG_SCSI_CMD;
	default:
		return TCMU_LOG_INFO;
	}
}

/* get the log level of tcmu-runner */
unsigned int tcmu_get_log_level(void)
{
	return (unsigned)tcmu_log_level;
}

void tcmu_set_log_level(int level)
{
	if (tcmu_log_level == to_syslog_level(level)) {
		tcmu_dbg("No changes to current log_level: %s, skipping it.\n",
		         log_level_lookup[level]);
		return;
	}
	if (level > TCMU_CONF_LOG_LEVEL_MAX)
		level = TCMU_CONF_LOG_LEVEL_MAX;
	else if (level < TCMU_CONF_LOG_LEVEL_MIN)
		level = TCMU_CONF_LOG_LEVEL_MIN;

	tcmu_crit("log level now is %s\n", log_level_lookup[level]);
	tcmu_log_level = to_syslog_level(level);
}

static void
log_internal(int pri, struct tcmu_device *dev, const char *funcname,
	     int linenr, const char *fmt, va_list args)
{
	if (pri > tcmu_log_level)
		return;

	if (!fmt)
		return;

	sys_vfprintf(stderr, fmt, args);
}

void tcmu_crit_message(struct tcmu_device *dev, const char *funcname,
		       int linenr, const char *fmt, ...)
{
	va_list args;

	va_start(args, fmt);
	log_internal(TCMU_LOG_CRIT, dev, funcname, linenr, fmt, args);
	va_end(args);
}

void tcmu_err_message(struct tcmu_device *dev, const char *funcname,
		      int linenr, const char *fmt, ...)
{
	va_list args;

	va_start(args, fmt);
	log_internal(TCMU_LOG_ERROR, dev, funcname, linenr, fmt, args);
	va_end(args);
}

void tcmu_warn_message(struct tcmu_device *dev, const char *funcname,
		       int linenr, const char *fmt, ...)
{
	va_list args;

	va_start(args, fmt);
	log_internal(TCMU_LOG_WARN, dev, funcname, linenr, fmt, args);
	va_end(args);
}

void tcmu_info_message(struct tcmu_device *dev, const char *funcname,
		       int linenr, const char *fmt, ...)
{
	va_list args;

	va_start(args, fmt);
	log_internal(TCMU_LOG_INFO, dev, funcname, linenr, fmt, args);
	va_end(args);
}

void tcmu_dbg_message(struct tcmu_device *dev, const char *funcname,
		      int linenr, const char *fmt, ...)
{
	va_list args;

	va_start(args, fmt);
	log_internal(TCMU_LOG_DEBUG, dev, funcname, linenr, fmt, args);
	va_end(args);
}

void tcmu_dbg_scsi_cmd_message(struct tcmu_device *dev, const char *funcname,
			       int linenr, const char *fmt, ...)
{
	va_list args;

	va_start(args, fmt);
	log_internal(TCMU_LOG_DEBUG_SCSI_CMD, dev, funcname, linenr, fmt,
		     args);
	va_end(args);
}
