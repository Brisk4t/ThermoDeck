#ifndef CORE_CONFIG_H
#define CORE_CONFIG_H

/* Minimal INI reader: [section] key = value, ';' or '#' comments. */
int config_load(const char *path);
const char *config_get(const char *section, const char *key, const char *def);
int config_int(const char *section, const char *key, int def);

#endif
