// license:BSD-3-Clause
// Nintendo Switch build: force-included in every file (-include). Declares the few POSIX functions MAME's
// unix OSD file calls and newlib/libnx does not declare in the header MAME uses. C++'s <cstdlib> and
// <cstring> reach the C headers through #include_next and step over shim headers, so a forced include is
// the only place that works. Implementations: switch_compat.cpp.
#ifndef TCVR_SWITCH_FORCED_H
#define TCVR_SWITCH_FORCED_H
#include <stddef.h>
#include <sys/types.h>
#ifdef __cplusplus
extern "C" {
#endif
int setenv(const char *name, const char *value, int overwrite);
int kill(pid_t pid, int sig);
int strerror_r(int errnum, char *buf, size_t buflen);
#ifdef __cplusplus
}
#endif
#endif
