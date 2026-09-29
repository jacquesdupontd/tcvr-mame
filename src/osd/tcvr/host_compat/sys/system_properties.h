// license:BSD-3-Clause
// Nintendo Switch build: Android "system properties" (debug.tcvr.*) are read from a text file on the SD card,
// one "name=value" per line (default /switch/segarally/props.ini), and from the environment variable of the
// same name with dots replaced by underscores. A missing property behaves as on Android: empty string, 0.
#ifndef TCVR_SWITCH_SYSTEM_PROPERTIES_H
#define TCVR_SWITCH_SYSTEM_PROPERTIES_H

#define PROP_NAME_MAX 32
#define PROP_VALUE_MAX 92

#ifdef __cplusplus
extern "C" {
#endif
int __system_property_get(const char *name, char *value);
#ifdef __cplusplus
}
#endif

#endif
