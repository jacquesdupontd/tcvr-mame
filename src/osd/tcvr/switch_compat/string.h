// license:BSD-3-Clause
// Nintendo Switch build: declares the XSI strerror_r that asio calls (implemented in switch_compat.cpp).
#ifndef TCVR_SWITCH_STRING_H
#define TCVR_SWITCH_STRING_H
#include_next <string.h>
#ifdef __cplusplus
extern "C" {
#endif
int strerror_r(int errnum, char *buf, size_t buflen);
#ifdef __cplusplus
}
#endif
#endif
