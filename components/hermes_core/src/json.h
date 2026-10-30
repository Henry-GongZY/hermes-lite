#ifndef HL_JSON_H
#define HL_JSON_H

#include <stddef.h>

/* Arena-based JSON parser + fixed-capacity string builder.
 * No malloc inside the parser: everything is carved out of a caller-provided
 * arena, so a parse either fits or fails. Designed for small MCUs. */

typedef enum { J_NULL, J_BOOL, J_NUM, J_STR, J_ARR, J_OBJ } jtype_t;

typedef struct jval {
    jtype_t type;
    struct jval *child;   /* first child (J_ARR/J_OBJ) */
    struct jval *next;    /* next sibling */
    char *key;            /* object member key (arena string) */
    char *str;            /* J_STR: arena, NUL-terminated, UTF-8 decoded */
    double num;
    int boolean;
} jval_t;

/* Parse NUL-terminated src into arena (arena_cap bytes, must be writable).
 * Returns NULL on syntax error or arena exhaustion. If used != NULL it
 * receives the arena bytes consumed, so callers can parse consecutive
 * documents into one arena (arena + used for the next call). */
jval_t *hl_json_parse(char *arena, size_t arena_cap, const char *src, size_t *used);

jval_t *hl_jobj_get(const jval_t *obj, const char *key);
int     hl_jarr_len(const jval_t *arr);
jval_t *hl_jarr_get(const jval_t *arr, int index);

/* Fixed-capacity string builder; err is set on overflow and stays set. */
typedef struct { char *buf; size_t len, cap; int err; } sbuf_t;

void hl_sb_init(sbuf_t *sb, char *buf, size_t cap);
void hl_sb_putc(sbuf_t *sb, char c);
void hl_sb_puts(sbuf_t *sb, const char *s);
void hl_sb_printf(sbuf_t *sb, const char *fmt, ...);
/* Appends s as a quoted, escaped JSON string (including the quotes). */
void hl_sb_json_str(sbuf_t *sb, const char *s);

/* malloc'd copy of s as a quoted, escaped JSON string; NULL on OOM. */
char *hl_json_quote_dyn(const char *s);

#endif
