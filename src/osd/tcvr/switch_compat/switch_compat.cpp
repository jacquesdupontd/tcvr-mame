// license:BSD-3-Clause
// Implementation of the two Android APIs the TCVR sources use, for the Nintendo Switch build.
#include <stdlib.h>
#include <signal.h>
#include "android/log.h"
#include "sys/system_properties.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string.h>
#include <map>
#include <mutex>
#include <string>
#include <chrono>

namespace {

std::mutex s_lock;
std::map<std::string, std::string> s_props;
bool s_loaded = false;
FILE *s_log = nullptr;
std::chrono::steady_clock::time_point s_t0 = std::chrono::steady_clock::now();

char const *props_path()
{
	char const *p = getenv("TCVR_PROPS");
	return p && *p ? p : "/switch/segarally/props.ini";
}

char const *log_path()
{
	char const *p = getenv("TCVR_LOG");
	return p && *p ? p : "/switch/segarally/log.txt";
}

void load_locked()
{
	if (s_loaded) return;
	s_loaded = true;
	if (FILE *f = fopen(props_path(), "r")) {
		char line[256];
		while (fgets(line, sizeof line, f)) {
			char *eq = strchr(line, '=');
			if (!eq || line[0] == '#') continue;
			*eq = 0;
			char *v = eq + 1;
			size_t n = strlen(v);
			while (n && (v[n - 1] == '\n' || v[n - 1] == '\r' || v[n - 1] == ' ')) v[--n] = 0;
			s_props[line] = v;
		}
		fclose(f);
	}
}

}

extern "C" int __system_property_get(const char *name, char *value)
{
	std::lock_guard<std::mutex> g(s_lock);
	load_locked();
	value[0] = 0;
	auto it = s_props.find(name);
	if (it == s_props.end()) {
		std::string env = name;
		for (char &c : env) if (c == '.') c = '_';
		if (char const *e = getenv(env.c_str())) { strncpy(value, e, PROP_VALUE_MAX - 1); value[PROP_VALUE_MAX - 1] = 0; return int(strlen(value)); }
		return 0;
	}
	strncpy(value, it->second.c_str(), PROP_VALUE_MAX - 1);
	value[PROP_VALUE_MAX - 1] = 0;
	return int(strlen(value));
}

extern "C" int __android_log_vprint(int prio, const char *tag, const char *fmt, va_list ap)
{
	char msg[1024];
	vsnprintf(msg, sizeof msg, fmt, ap);
	return __android_log_write(prio, tag, msg);
}

extern "C" int __android_log_print(int prio, const char *tag, const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	int r = __android_log_vprint(prio, tag, fmt, ap);
	va_end(ap);
	return r;
}

// The log is queued in memory here (any thread) and drained by the application's main thread, which is the only
// one that touches the SD card and the text console: libnx's console is not thread-safe (it crashed at 10 s when
// the emulation thread printed to it, 29/09) and a log file held open made the bridge's reads fail with EIO.
static std::string s_queue;

extern "C" int __android_log_write(int prio, const char *tag, const char *text)
{
	static char const letters[] = "?-VDIWEF-";
	double t = std::chrono::duration<double>(std::chrono::steady_clock::now() - s_t0).count();
	char line[1200];
	int n = snprintf(line, sizeof line, "%9.3f %c %s: %s\n", t, letters[prio < 0 || prio > 8 ? 0 : prio], tag, text);
	std::lock_guard<std::mutex> g(s_lock);
	if (s_queue.size() < (8u << 20)) s_queue += line;
	return n;
}

// Takes everything queued so far (called by the main thread).
extern "C" size_t tcvr_switch_log_take(char *out, size_t cap)
{
	std::lock_guard<std::mutex> g(s_lock);
	size_t n = s_queue.size() < cap ? s_queue.size() : cap;
	memcpy(out, s_queue.data(), n);
	s_queue.erase(0, n);
	return n;
}

#include <netdb.h>
#include "dlfcn.h"

extern "C" void *dlopen(const char *, int) { return nullptr; }
extern "C" void *dlsym(void *, const char *) { return nullptr; }
extern "C" int dlclose(void *) { return 0; }
extern "C" char *dlerror(void) { static char m[] = "dynamic loading is not available on the Nintendo Switch"; return m; }
extern "C" struct hostent *gethostbyname(const char *) { return nullptr; }

extern "C" int strerror_r(int e, char *buf, size_t n)
{
	snprintf(buf, n, "%s", strerror(e));
	return 0;
}

#include <malloc.h>
#include "sys/mman.h"

extern "C" void *mmap(void *, size_t len, int, int, int, off_t)
{
	void *p = memalign(0x1000, (len + 0xfff) & ~size_t(0xfff));
	if (!p) return MAP_FAILED;
	memset(p, 0, len);
	return p;
}
extern "C" int munmap(void *addr, size_t) { free(addr); return 0; }
extern "C" int mprotect(void *, size_t, int) { return 0; }

extern "C" int setenv(const char *name, const char *value, int overwrite)
{
	if (!overwrite && getenv(name)) return 0;
	std::string kv = std::string(name) + "=" + value;
	return putenv(strdup(kv.c_str()));
}
extern "C" int kill(pid_t, int) { return -1; }

// ---- POSIX functions newlib leaves out, used by MAME's file layer and libc ----
#include <unistd.h>
#include <reent.h>
#include <switch.h>

extern "C" ssize_t pread(int fd, void *buf, size_t n, off_t off)
{
	off_t const here = lseek(fd, 0, SEEK_CUR);
	if (here < 0 || lseek(fd, off, SEEK_SET) < 0) return -1;
	ssize_t const r = read(fd, buf, n);
	lseek(fd, here, SEEK_SET);
	return r;
}
extern "C" ssize_t pwrite(int fd, const void *buf, size_t n, off_t off)
{
	off_t const here = lseek(fd, 0, SEEK_CUR);
	if (here < 0 || lseek(fd, off, SEEK_SET) < 0) return -1;
	ssize_t const r = write(fd, buf, n);
	lseek(fd, here, SEEK_SET);
	return r;
}
extern "C" long sysconf(int name)
{
	switch (name) {
	case _SC_PAGESIZE: return 0x1000;
	case _SC_NPROCESSORS_ONLN:
	case _SC_NPROCESSORS_CONF: return 3;   // an application gets cores 0-2; core 3 belongs to the system
	default: return -1;
	}
}
extern "C" int _getentropy_r(struct _reent *, void *buf, size_t len)
{
	randomGet(buf, len);
	return 0;
}
