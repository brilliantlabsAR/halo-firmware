/*
** $Id: lport.c $
** zephyr port of lua
*/

#define lport_c
#define LUA_CORE

#include <sys/time.h>
#include <sys/times.h>
#include <sys/types.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/console/console.h>

#include "lport.h"

clock_t times(struct tms *buf)
{
	if (buf == NULL) {
		return -1;
	}

	buf->tms_utime = k_uptime_get();
	buf->tms_stime = 0;
	buf->tms_cutime = 0;
	buf->tms_cstime = 0;

	return 0;
}

#ifndef CONFIG_TIMER
// fake time calls
int gettimeofday(struct timeval *restrict tv, void *restrict tz)
{
	ARG_UNUSED(tz);
	tv->tv_sec = k_uptime_get() / 1000;
	tv->tv_usec = k_uptime_get() % 1000;

	return 0;
}

#endif

#ifndef CONFIG_POSIX_FS
// fake file system calls
int open(const char *pathname, int flags, ...)
{
	return 0;
}

int close(int fd)
{
	return 0;
}

ssize_t write(int fd, const void *buf, size_t count)
{
	(void)fd; /* Not used, avoid warning */
	return count;
}

ssize_t read(int fd, void *buf, size_t count)
{
	return 0;
}

off_t lseek(int fd, off_t offset, int whence)
{
	return 0;
}

int rename(const char *old, const char *new)
{
	return 0;
}

int unlink(const char *pathname)
{
	return 0;
}

#endif