// license:BSD-3-Clause
// Nintendo Switch build: setenv is not declared by newlib's <stdlib.h> in this configuration.
#ifndef TCVR_SWITCH_STDLIB_H
#define TCVR_SWITCH_STDLIB_H
#include_next <stdlib.h>
#ifdef __cplusplus
extern "C" {
#endif
int setenv(const char *name, const char *value, int overwrite);
#ifdef __cplusplus
}
#endif
#endif
