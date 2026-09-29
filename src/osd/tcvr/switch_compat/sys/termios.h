// license:BSD-3-Clause
// Nintendo Switch build: newlib's <termios.h> includes this file, which libnx does not ship. Terminals
// do not exist on the console; asio only needs the header to be includable.
#ifndef TCVR_SWITCH_SYS_TERMIOS_H
#define TCVR_SWITCH_SYS_TERMIOS_H
#include <sys/types.h>
typedef unsigned int tcflag_t;
typedef unsigned char cc_t;
typedef unsigned int speed_t;
#define NCCS 32
struct termios { tcflag_t c_iflag, c_oflag, c_cflag, c_lflag; cc_t c_cc[NCCS]; speed_t c_ispeed, c_ospeed; };
#endif
