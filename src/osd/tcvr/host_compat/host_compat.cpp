// license:BSD-3-Clause
// Native Linux build of the TCVR core (offline oracle for the Switch renderer): the two Android APIs the TCVR
// sources use. Properties come from the environment (debug.tcvr.x.y -> debug_tcvr_x_y) or from the file named by
// TCVR_PROPS; logging goes to stderr.
#include "android/log.h"
#include "sys/system_properties.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <string>

namespace {
std::mutex s_lock;
std::map<std::string, std::string> s_props;
bool s_loaded = false;
void load_locked()
{
	if (s_loaded) return;
	s_loaded = true;
	char const *path = getenv("TCVR_PROPS");
	if (!path) return;
	if (FILE *f = fopen(path, "r")) {
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
	va_list ap; va_start(ap, fmt);
	int r = __android_log_vprint(prio, tag, fmt, ap);
	va_end(ap);
	return r;
}
extern "C" int __android_log_write(int prio, const char *tag, const char *text)
{
	std::lock_guard<std::mutex> g(s_lock);
	return fprintf(stderr, "%c %s: %s\n", "?-VDIWEF-"[prio < 0 || prio > 8 ? 0 : prio], tag, text);
}
