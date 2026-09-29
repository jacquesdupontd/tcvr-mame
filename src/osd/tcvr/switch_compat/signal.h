// license:BSD-3-Clause
// Nintendo Switch build: there is no other process to signal; kill() is declared for MAME's unix OSD file
// and always fails.
#ifndef TCVR_SWITCH_SIGNAL_H
#define TCVR_SWITCH_SIGNAL_H
#include_next <signal.h>
#include <sys/types.h>
#ifdef __cplusplus
extern "C" {
#endif
int kill(pid_t pid, int sig);
#ifdef __cplusplus
}
#endif
#endif
