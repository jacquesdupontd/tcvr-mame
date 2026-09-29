// license:BSD-3-Clause
// Nintendo Switch build: newlib/libnx has no Unix-domain sockets. asio and MAME's posix socket file only
// need the type to compile; nothing on the Switch opens such a socket.
#ifndef TCVR_SWITCH_UN_H
#define TCVR_SWITCH_UN_H
#include <sys/socket.h>
struct sockaddr_un { sa_family_t sun_family; char sun_path[108]; };
#endif
