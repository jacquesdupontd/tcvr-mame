// license:BSD-3-Clause
// Nintendo Switch build: a few POSIX error numbers asio names that newlib does not define.
#ifndef TCVR_SWITCH_ERRNO_H
#define TCVR_SWITCH_ERRNO_H
#include_next <errno.h>
#ifndef ESHUTDOWN
#define ESHUTDOWN 110
#endif
#endif
