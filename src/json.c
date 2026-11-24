/*
 * Tiny non-validating JSON tokenizer (jsmn style). json_parse() turns the text into a
 * flat array of tokens in document order, each with its byte range and parent; the
 * accessors below walk that array. Strings are not copied until json_str() is called.
 */

#include "json.h"

#include <stdlib.h>
#include <string.h>

/* Appends a token; returns its index or -1 if the token array is full. */
static int new_tok(json_doc *d, int max, jtype type, int start, int end, int parent)
{
    if (d->n >= max) return -1;
    jtok *t = &d->t[d->n];
    t->type = type;
    t->start = start;
    t->end = end;
    t->size = 0;
    t->parent = parent;
    return d->n++;
}

/* Tokenizes js[0..len). Returns the token count, or <0 on overflow / malformed input. */
int json_parse(json_doc *d, const char *js, size_t len, jtok *toks, int max)
{
    d->js = js;
    d->t = toks;
    d->n = 0;
    int cur = -1; /* current open container */
    size_t i = 0;
    while (i < len) {
        char c = js[i];
        switch (c) {
        /* open a container; it becomes the parent of what follows */
        case '{':
        case '[': {
            int k = new_tok(d, max, c == '{' ? J_OBJ : J_ARR, (int)i, -1, cur);
            if (k < 0) return -1;
            if (cur >= 0) d->t[cur].size++;
            cur = k;
            i++;
            break;
        }
        /* close the current container and pop back to its parent */
        case '}':
        case ']': {
            jtype want = c == '}' ? J_OBJ : J_ARR;
            if (cur < 0 || d->t[cur].type != want) return -2;
            d->t[cur].end = (int)i + 1;
            cur = d->t[cur].parent;
            i++;
            break;
        }
        /* string: token covers the text between the quotes (escapes left as-is) */
        case '"': {
            size_t s = ++i;
            while (i < len && js[i] != '"') {
                if (js[i] == '\\' && i + 1 < len) i++;
                i++;
            }
            if (i >= len) return -3;
            int k = new_tok(d, max, J_STR, (int)s, (int)i, cur);
            if (k < 0) return -1;
            if (cur >= 0) d->t[cur].size++;
            i++;
            break;
        }
        case ' ':
        case '\t':
        case '\r':
        case '\n':
        case ':':
        case ',': i++; break;
        /* number / true / false / null */
        default: {
            size_t s = i;
            while (i < len && !strchr(" \t\r\n,:]}", js[i])) i++;
            int k = new_tok(d, max, J_PRIM, (int)s, (int)i, cur);
            if (k < 0) return -1;
            if (cur >= 0) d->t[cur].size++;
            break;
        }
        }
    }
    /* every container must have been closed */
    for (int k = 0; k < d->n; k++)
        if ((d->t[k].type == J_OBJ || d->t[k].type == J_ARR) && d->t[k].end < 0) return -4;
    return d->n;
}

/* Index of the token following the whole subtree of token i. */
static int skip(const json_doc *d, int i)
{
    int end = d->t[i].end;
    if (d->t[i].type == J_STR) end++; /* closing quote */
    int j = i + 1;
    while (j < d->n && d->t[j].start < end) j++;
    return j;
}

/* Value token for key in object obj, or -1. Walks key/value pairs, skipping nested values. */
int json_get(const json_doc *d, int obj, const char *key)
{
    if (obj < 0 || obj >= d->n || d->t[obj].type != J_OBJ) return -1;
    size_t kl = strlen(key);
    int j = obj + 1;
    while (j + 1 < d->n && d->t[j].start < d->t[obj].end) {
        const jtok *kt = &d->t[j];
        if (kt->type == J_STR && (size_t)(kt->end - kt->start) == kl && !memcmp(d->js + kt->start, key, kl))
            return j + 1;
        j = skip(d, j + 1);
    }
    return -1;
}

/* Like json_get but follows a dotted path, e.g. "album.name". */
int json_path(const json_doc *d, int obj, const char *path)
{
    char part[64];
    while (obj >= 0 && *path) {
        size_t l = strcspn(path, ".");
        if (l >= sizeof part) return -1;
        memcpy(part, path, l);
        part[l] = 0;
        obj = json_get(d, obj, part);
        path += l;
        if (*path == '.') path++;
    }
    return obj;
}

/* n-th element of array arr, or -1. */
int json_at(const json_doc *d, int arr, int n)
{
    if (arr < 0 || arr >= d->n || d->t[arr].type != J_ARR || n >= d->t[arr].size) return -1;
    int j = arr + 1;
    while (n-- > 0) j = skip(d, j);
    return j < d->n ? j : -1;
}

/* Appends code point cp to out as UTF-8 (dropped if it doesn't fit). */
static void put_utf8(char *out, size_t outlen, size_t *o, unsigned cp)
{
    char b[4];
    int n;
    if (cp < 0x80) {
        b[0] = (char)cp;
        n = 1;
    }
    else if (cp < 0x800) {
        b[0] = (char)(0xC0 | (cp >> 6));
        b[1] = (char)(0x80 | (cp & 63));
        n = 2;
    }
    else if (cp < 0x10000) {
        b[0] = (char)(0xE0 | (cp >> 12));
        b[1] = (char)(0x80 | ((cp >> 6) & 63));
        b[2] = (char)(0x80 | (cp & 63));
        n = 3;
    }
    else {
        b[0] = (char)(0xF0 | (cp >> 18));
        b[1] = (char)(0x80 | ((cp >> 12) & 63));
        b[2] = (char)(0x80 | ((cp >> 6) & 63));
        b[3] = (char)(0x80 | (cp & 63));
        n = 4;
    }
    if (*o + (size_t)n >= outlen) return;
    memcpy(out + *o, b, (size_t)n);
    *o += (size_t)n;
}

/* Parses 4 hex digits (for \uXXXX escapes). */
static unsigned hex4(const char *s)
{
    unsigned v = 0;
    for (int i = 0; i < 4; i++) {
        char c = s[i];
        v <<= 4;
        if (c >= '0' && c <= '9')
            v |= (unsigned)(c - '0');
        else if (c >= 'a' && c <= 'f')
            v |= (unsigned)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F')
            v |= (unsigned)(c - 'A' + 10);
    }
    return v;
}

/* Copies string token i into out, decoding escapes (including UTF-16 surrogate pairs). */
int json_str(const json_doc *d, int i, char *out, size_t outlen)
{
    if (!outlen) return -1;
    out[0] = 0;
    if (i < 0 || i >= d->n || d->t[i].type != J_STR) return -1;
    const char *s = d->js + d->t[i].start, *e = d->js + d->t[i].end;
    size_t o = 0;
    while (s < e && o + 1 < outlen) {
        if (*s != '\\') {
            out[o++] = *s++;
            continue;
        }
        s++;
        if (s >= e) break;
        char c = *s++;
        switch (c) {
        case 'n': out[o++] = '\n'; break;
        case 't': out[o++] = '\t'; break;
        case 'r': out[o++] = '\r'; break;
        case 'b': out[o++] = '\b'; break;
        case 'f': out[o++] = '\f'; break;
        case 'u':
            if (e - s >= 4) {
                unsigned cp = hex4(s);
                s += 4;
                if (cp >= 0xD800 && cp < 0xDC00 && e - s >= 6 && s[0] == '\\' && s[1] == 'u') {
                    unsigned lo = hex4(s + 2);
                    if (lo >= 0xDC00 && lo < 0xE000) {
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                        s += 6;
                    }
                }
                put_utf8(out, outlen, &o, cp);
            }
            break;
        default: out[o++] = c; break;
        }
    }
    out[o] = 0;
    return 0;
}

/* Numeric value of a primitive token, or def if it isn't a number. */
long long json_int(const json_doc *d, int i, long long def)
{
    if (i < 0 || i >= d->n || d->t[i].type != J_PRIM) return def;
    char c = d->js[d->t[i].start];
    if (c != '-' && (c < '0' || c > '9')) return def;
    return strtoll(d->js + d->t[i].start, NULL, 10);
}

/* true/false primitive, or def. */
int json_bool(const json_doc *d, int i, int def)
{
    if (i < 0 || i >= d->n || d->t[i].type != J_PRIM) return def;
    char c = d->js[d->t[i].start];
    return c == 't' ? 1 : c == 'f' ? 0 : def;
}

/* Missing (-1) counts as null too. */
int json_is_null(const json_doc *d, int i) { return i < 0 || (d->t[i].type == J_PRIM && d->js[d->t[i].start] == 'n'); }
