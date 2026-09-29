// license:BSD-3-Clause
// Nintendo Switch build: no dynamic loading. The functions exist (implemented in switch_compat.cpp) and
// always fail, so MAME's optional dynamic modules are simply absent.
#ifndef TCVR_SWITCH_DLFCN_H
#define TCVR_SWITCH_DLFCN_H
#define RTLD_LAZY 1
#define RTLD_NOW 2
#define RTLD_GLOBAL 4
#define RTLD_LOCAL 0
#ifdef __cplusplus
extern "C" {
#endif
void *dlopen(const char *file, int mode);
void *dlsym(void *handle, const char *name);
int dlclose(void *handle);
char *dlerror(void);
#ifdef __cplusplus
}
#endif
#endif
