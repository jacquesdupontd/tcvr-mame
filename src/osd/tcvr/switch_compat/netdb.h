// license:BSD-3-Clause
// Nintendo Switch build: libnx provides getaddrinfo but does not declare gethostbyname, which MAME's socket
// file still references. Declared here, implemented (always failing) in switch_compat.cpp.
#ifndef TCVR_SWITCH_NETDB_H
#define TCVR_SWITCH_NETDB_H
#include_next <netdb.h>
#ifdef __cplusplus
extern "C" {
#endif
struct hostent *gethostbyname(const char *name);
#ifdef __cplusplus
}
#endif
#endif
