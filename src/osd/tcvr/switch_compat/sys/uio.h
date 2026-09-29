// license:BSD-3-Clause
// Nintendo Switch build: newlib/libnx has no <sys/uio.h>; struct iovec comes with <sys/socket.h>.
#ifndef TCVR_SWITCH_UIO_H
#define TCVR_SWITCH_UIO_H
#include <sys/socket.h>
#endif
