/*
 * Minimal INI reader for thermodeck.ini: [section] headers and key = value lines.
 * Everything is loaded once at startup into a fixed table; lookups are case-insensitive.
 */

#include "config.h"

#include <ctype.h>
#include <stdlib.h>
#include <strings.h>

#include "common.h"

#define MAX_ENTRIES 96

static struct {
    char section[32], key[48], value[256];
} entries[MAX_ENTRIES];
static int n_entries;

/* Strips leading/trailing whitespace in place. */
static char *trim(char *s)
{
    while (isspace((unsigned char)*s)) s++;
    char *e = s + strlen(s);
    while (e > s && isspace((unsigned char)e[-1])) *--e = 0;
    return s;
}

/* Parses the file into entries[]. Comments start with ; or #, values may be quoted. */
int config_load(const char *path)
{
    char *data = file_read(path, NULL);
    if (!data) return -1;
    char section[32] = "";
    n_entries = 0;
    for (char *line = strtok(data, "\n"); line; line = strtok(NULL, "\n")) {
        char *l = trim(line);
        if (!*l || *l == ';' || *l == '#') continue;
        /* section header */
        if (*l == '[') {
            char *e = strchr(l, ']');
            if (e) *e = 0;
            str_copy(section, sizeof section, trim(l + 1));
            continue;
        }
        /* key = value (unquote "..." or '...') */
        char *eq = strchr(l, '=');
        if (!eq || n_entries >= MAX_ENTRIES) continue;
        *eq = 0;
        char *v = trim(eq + 1);
        size_t vl = strlen(v);
        if (vl >= 2 && ((v[0] == '"' && v[vl - 1] == '"') || (v[0] == '\'' && v[vl - 1] == '\''))) {
            v[vl - 1] = 0;
            v++;
        }
        str_copy(entries[n_entries].section, sizeof entries[0].section, section);
        str_copy(entries[n_entries].key, sizeof entries[0].key, trim(l));
        str_copy(entries[n_entries].value, sizeof entries[0].value, v);
        n_entries++;
    }
    free(data);
    return 0;
}

/* Raw string value, or def if the key is missing. */
const char *config_get(const char *section, const char *key, const char *def)
{
    for (int i = 0; i < n_entries; i++)
        if (!strcasecmp(entries[i].section, section) && !strcasecmp(entries[i].key, key)) return entries[i].value;
    return def;
}

/* Integer value; also accepts true/false, yes/no, on/off. */
int config_int(const char *section, const char *key, int def)
{
    const char *v = config_get(section, key, NULL);
    if (!v || !*v) return def;
    if (!strcasecmp(v, "true") || !strcasecmp(v, "yes") || !strcasecmp(v, "on")) return 1;
    if (!strcasecmp(v, "false") || !strcasecmp(v, "no") || !strcasecmp(v, "off")) return 0;
    return atoi(v);
}
