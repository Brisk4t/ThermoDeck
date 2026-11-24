#ifndef JSON_JSON_H
#define JSON_JSON_H
#include <stddef.h>

/* Tiny jsmn-style JSON tokenizer: tokens are in document order, a token's
 * children follow it directly. */
typedef enum { J_UNDEF = 0, J_OBJ, J_ARR, J_STR, J_PRIM } jtype;
typedef struct {
    jtype type;
    int start, end; /* byte range in source (strings: without quotes) */
    int size;       /* direct child tokens (object: keys + values) */
    int parent;
} jtok;

typedef struct {
    const char *js;
    jtok *t;
    int n;
} json_doc;

/* Returns number of tokens, or <0 on error. */
int json_parse(json_doc *d, const char *js, size_t len, jtok *toks, int max);
/* Value token for key in object `obj`, or -1. */
int json_get(const json_doc *d, int obj, const char *key);
/* Value token for a dotted path like "item.album.name" starting at obj. */
int json_path(const json_doc *d, int obj, const char *path);
/* Token after the whole subtree of token i (to walk object members: key = obj + 1,
 * value = key + 1, next key = json_next(value)). */
int json_next(const json_doc *d, int i);
/* n-th element of array, or -1. */
int json_at(const json_doc *d, int arr, int n);
/* Unescaped string value (UTF-8). Returns 0 if token is a string. */
int json_str(const json_doc *d, int i, char *out, size_t outlen);
long long json_int(const json_doc *d, int i, long long def);
int json_bool(const json_doc *d, int i, int def);
int json_is_null(const json_doc *d, int i);

#endif
