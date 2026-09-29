// license:BSD-3-Clause
// Nintendo Switch build: no mmap in newlib/libnx. MAME's virtual-memory helper (osdlib_unix.cpp) only uses it
// for the recompiler's code cache and big arenas; here anonymous mappings are plain aligned allocations and
// protections are no-ops (nothing is executable: the recompilers are disabled, all CPUs are interpreted).
#ifndef TCVR_SWITCH_MMAN_H
#define TCVR_SWITCH_MMAN_H
#include <stddef.h>
#include <sys/types.h>
#define PROT_NONE  0
#define PROT_READ  1
#define PROT_WRITE 2
#define PROT_EXEC  4
#define MAP_SHARED  1
#define MAP_PRIVATE 2
#define MAP_ANON    0x20
#define MAP_ANONYMOUS MAP_ANON
#define MAP_FAILED ((void *)-1)
#ifdef __cplusplus
extern "C" {
#endif
void *mmap(void *addr, size_t len, int prot, int flags, int fd, off_t off);
int munmap(void *addr, size_t len);
int mprotect(void *addr, size_t len, int prot);
#ifdef __cplusplus
}
#endif
#endif
