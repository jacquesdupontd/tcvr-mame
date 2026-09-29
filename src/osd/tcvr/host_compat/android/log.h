// license:BSD-3-Clause
// Nintendo Switch build: the TCVR sources log through Android's logcat API. On the Switch the same calls
// go to stdout (nxlink -s) and to a log file on the SD card, so the instrumentation written for the Quest
// (scheduler counters, audio, pacing) is compiled unchanged and measures the same things.
#ifndef TCVR_SWITCH_ANDROID_LOG_H
#define TCVR_SWITCH_ANDROID_LOG_H

#include <stdarg.h>

enum { ANDROID_LOG_UNKNOWN = 0, ANDROID_LOG_DEFAULT, ANDROID_LOG_VERBOSE, ANDROID_LOG_DEBUG,
       ANDROID_LOG_INFO, ANDROID_LOG_WARN, ANDROID_LOG_ERROR, ANDROID_LOG_FATAL, ANDROID_LOG_SILENT };

#ifdef __cplusplus
extern "C" {
#endif
int __android_log_print(int prio, const char *tag, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
int __android_log_vprint(int prio, const char *tag, const char *fmt, va_list ap);
int __android_log_write(int prio, const char *tag, const char *text);
#ifdef __cplusplus
}
#endif

#endif
#define TCVR_NO_NAMCOS22 1   // the Namco System 22 driver is not part of this build
