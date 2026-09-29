// license:BSD-3-Clause
// Nintendo Switch build: -D__ANDROID__ is defined so the Quest instrumentation compiles unchanged;
// asio then looks for the NDK level header. The value only has to be high enough for asio's checks.
#ifndef TCVR_SWITCH_API_LEVEL_H
#define TCVR_SWITCH_API_LEVEL_H
#define __ANDROID_API__ 24
#endif
