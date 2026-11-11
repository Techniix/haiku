/* Haiku overlay: extends compat/sys/syslog.h with the remaining syslog(3)
 * priorities (values as FreeBSD sys/syslog.h).
 * Copyright 2026, LinuxKPI_HaikuOS contributors. MIT. */
#ifndef _LKPI_HAIKU_SYS_SYSLOG_H_
#define _LKPI_HAIKU_SYS_SYSLOG_H_
#include_next <sys/syslog.h>
#ifndef LOG_EMERG
#define LOG_EMERG	0
#endif
#ifndef LOG_ALERT
#define LOG_ALERT	1
#endif
#ifndef LOG_CRIT
#define LOG_CRIT	2
#endif
#ifndef LOG_NOTICE
#define LOG_NOTICE	5
#endif
#ifndef LOG_INFO
#define LOG_INFO	6
#endif
#ifndef LOG_DEBUG
#define LOG_DEBUG	7
#endif
#endif
