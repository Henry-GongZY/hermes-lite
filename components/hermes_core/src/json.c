#include "json.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>

/* ---------------- arena parser ---------------- */

#define J_MAX_DEPTH 40

typedef struct {
    char  *a;
    size_t cap, pos;
    int    err;
} jp_t;

typedef struct {
    const char *s;
    size_t n, i;
    jp_t  *a;
    int    depth;
} jctx_t;

static void *ja(jp_t *p, size_t n)
{
    if (p->err)
        return NULL;
    n = (n + 7) & ~(size_t)7;
    if (p->pos + n > p->cap) {
        p->err = 1;
        return NULL;
    }
    void *r = p->a + p->pos;
    p->pos += n;
    return r;
}

static void skipws(jctx_t *c)
{
    while (c->i < c->n) {
        char ch = c->s[c->i];
        if (ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r')
            c->i++;
        else
            break;
    }
}

static jval_t *newval(jctx_t *c)
{
    jval_t *v = ja(c->a, sizeof *v);
    if (v)
        memset(v, 0, sizeof *v);
    return v;
}

static int hexv(int ch)
{
    if (ch >= '0' && ch <= '9') return ch - '0';
    if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
    if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
    return -1;
}

static int utf8_put(unsigned cp, char *out)
{
    if (cp < 0x80) {
        out[0] = (char)cp;
        return 1;
    }
    if (cp < 0x800) {
        out[0] = (char)(0xC0 | (cp >> 6));
        out[1] = (char)(0x80 | (cp & 0x3F));
        return 2;
    }
    if (cp < 0x10000) {
        out[0] = (char)(0xE0 | (cp >> 12));
        out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[2] = (char)(0x80 | (cp & 0x3F));
        return 3;
    }
    out[0] = (char)(0xF0 | (cp >> 18));
    out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
    out[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
    out[3] = (char)(0x80 | (cp & 0x3F));
    return 4;
}

/* Parse a string (opening quote already NOT consumed; caller handles).
 * Returns an arena NUL-terminated decoded string, or NULL on error. */
static char *parse_string_raw(jctx_t *c)
{
    if (c->i >= c->n || c->s[c->i] != '"')
        return NULL;
    c->i++;
    size_t start_i = c->i;

    int esc = 0;
    for (;;) {
        if (c->i >= c->n)
            return NULL;
        char ch = c->s[c->i];
        if (ch == '"')
            break;
        if (ch == '\\') {
            esc = 1;
            c->i += 2;
            continue;
        }
        c->i++;
    }
    size_t raw_len = c->i - start_i;
    c->i++; /* closing quote */

    jp_t *a = c->a;
    size_t alloc_at = a->pos;
    char *out = ja(a, raw_len + 1);
    if (!out)
        return NULL;

    size_t w = 0;
    if (!esc) {
        memcpy(out, c->s + start_i, raw_len);
        w = raw_len;
    } else {
        size_t k = 0;
        while (k < raw_len) {
            char ch = c->s[start_i + k];
            if (ch != '\\') {
                out[w++] = ch;
                k++;
                continue;
            }
            k++;
            if (k >= raw_len)
                return NULL;
            char e = c->s[start_i + k++];
            switch (e) {
            case '"':  out[w++] = '"';  break;
            case '\\': out[w++] = '\\'; break;
            case '/':  out[w++] = '/';  break;
            case 'b':  out[w++] = '\b'; break;
            case 'f':  out[w++] = '\f'; break;
            case 'n':  out[w++] = '\n'; break;
            case 'r':  out[w++] = '\r'; break;
            case 't':  out[w++] = '\t'; break;
            case 'u': {
                if (k + 4 > raw_len)
                    return NULL;
                unsigned cp = 0;
                for (int q = 0; q < 4; q++) {
                    int h = hexv((unsigned char)c->s[start_i + k + q]);
                    if (h < 0)
                        return NULL;
                    cp = cp * 16 + (unsigned)h;
                }
                k += 4;
                if (cp >= 0xD800 && cp <= 0xDBFF && k + 6 <= raw_len &&
                    c->s[start_i + k] == '\\' && c->s[start_i + k + 1] == 'u') {
                    unsigned lo = 0;
                    int ok = 1;
                    for (int q = 0; q < 4; q++) {
                        int h = hexv((unsigned char)c->s[start_i + k + 2 + q]);
                        if (h < 0) { ok = 0; break; }
                        lo = lo * 16 + (unsigned)h;
                    }
                    if (!ok || lo < 0xDC00 || lo > 0xDFFF)
                        return NULL;
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    k += 6;
                }
                w += (size_t)utf8_put(cp, out + w);
                break;
            }
            default:
                return NULL;
            }
        }
    }
    out[w] = 0;

    /* shrink arena tail so strings don't waste their worst-case reservation */
    size_t used = (w + 1 + 7) & ~(size_t)7;
    a->pos = alloc_at + used;
    return out;
}

static jval_t *parse_value(jctx_t *c)
{
    if (c->depth > J_MAX_DEPTH)
        return NULL;
    skipws(c);
    if (c->i >= c->n)
        return NULL;
    char ch = c->s[c->i];
    jval_t *v;

    if (ch == '{') {
        c->i++;
        c->depth++;
        v = newval(c);
        if (!v)
            return NULL;
        v->type = J_OBJ;
        skipws(c);
        if (c->i < c->n && c->s[c->i] == '}') {
            c->i++;
            c->depth--;
            return v;
        }
        jval_t *tail = NULL;
        for (;;) {
            skipws(c);
            char *key = parse_string_raw(c);
            if (!key)
                return NULL;
            skipws(c);
            if (c->i >= c->n || c->s[c->i] != ':')
                return NULL;
            c->i++;
            jval_t *val = parse_value(c);
            if (!val)
                return NULL;
            val->key = key;
            if (tail)
                tail->next = val;
            else
                v->child = val;
            tail = val;
            skipws(c);
            if (c->i < c->n && c->s[c->i] == ',') {
                c->i++;
                continue;
            }
            if (c->i < c->n && c->s[c->i] == '}') {
                c->i++;
                c->depth--;
                return v;
            }
            return NULL;
        }
    }

    if (ch == '[') {
        c->i++;
        c->depth++;
        v = newval(c);
        if (!v)
            return NULL;
        v->type = J_ARR;
        skipws(c);
        if (c->i < c->n && c->s[c->i] == ']') {
            c->i++;
            c->depth--;
            return v;
        }
        jval_t *tail = NULL;
        for (;;) {
            jval_t *val = parse_value(c);
            if (!val)
                return NULL;
            if (tail)
                tail->next = val;
            else
                v->child = val;
            tail = val;
            skipws(c);
            if (c->i < c->n && c->s[c->i] == ',') {
                c->i++;
                continue;
            }
            if (c->i < c->n && c->s[c->i] == ']') {
                c->i++;
                c->depth--;
                return v;
            }
            return NULL;
        }
    }

    if (ch == '"') {
        v = newval(c);
        if (!v)
            return NULL;
        v->type = J_STR;
        v->str = parse_string_raw(c);
        return v->str ? v : NULL;
    }

    if (ch == 't') {
        if (c->n - c->i < 4 || strncmp(c->s + c->i, "true", 4))
            return NULL;
        c->i += 4;
        v = newval(c);
        if (!v)
            return NULL;
        v->type = J_BOOL;
        v->boolean = 1;
        return v;
    }
    if (ch == 'f') {
        if (c->n - c->i < 5 || strncmp(c->s + c->i, "false", 5))
            return NULL;
        c->i += 5;
        v = newval(c);
        if (!v)
            return NULL;
        v->type = J_BOOL;
        return v;
    }
    if (ch == 'n') {
        if (c->n - c->i < 4 || strncmp(c->s + c->i, "null", 4))
            return NULL;
        c->i += 4;
        v = newval(c);
        if (!v)
            return NULL;
        v->type = J_NULL;
        return v;
    }

    if (ch == '-' || (ch >= '0' && ch <= '9')) {
        char tmp[64];
        size_t k = 0;
        while (c->i < c->n && k < sizeof(tmp) - 1) {
            char d = c->s[c->i];
            if ((d >= '0' && d <= '9') || d == '-' || d == '+' || d == '.' ||
                d == 'e' || d == 'E') {
                tmp[k++] = d;
                c->i++;
            } else {
                break;
            }
        }
        tmp[k] = 0;
        char *endp = NULL;
        double d = strtod(tmp, &endp);
        if (endp == tmp || *endp)
            return NULL;
        v = newval(c);
        if (!v)
            return NULL;
        v->type = J_NUM;
        v->num = d;
        return v;
    }
    return NULL;
}

jval_t *hl_json_parse(char *arena, size_t arena_cap, const char *src, size_t *used)
{
    jp_t a = { arena, arena_cap, 0, 0 };
    jctx_t c = { src, strlen(src), 0, &a, 0 };
    jval_t *v = parse_value(&c);
    if (!v || a.err)
        return NULL;
    skipws(&c);
    if (c.i != c.n)
        return NULL; /* trailing garbage */
    if (used)
        *used = a.pos;
    return v;
}

jval_t *hl_jobj_get(const jval_t *obj, const char *key)
{
    if (!obj || obj->type != J_OBJ)
        return NULL;
    for (jval_t *m = obj->child; m; m = m->next)
        if (m->key && !strcmp(m->key, key))
            return m;
    return NULL;
}

int hl_jarr_len(const jval_t *arr)
{
    if (!arr || arr->type != J_ARR)
        return 0;
    int n = 0;
    for (jval_t *m = arr->child; m; m = m->next)
        n++;
    return n;
}

jval_t *hl_jarr_get(const jval_t *arr, int index)
{
    if (!arr || arr->type != J_ARR)
        return NULL;
    jval_t *m = arr->child;
    while (index-- > 0 && m)
        m = m->next;
    return m;
}

/* ---------------- string builder ---------------- */

void hl_sb_init(sbuf_t *sb, char *buf, size_t cap)
{
    sb->buf = buf;
    sb->cap = cap;
    sb->len = 0;
    sb->err = 0;
    if (cap)
        buf[0] = 0;
}

void hl_sb_putc(sbuf_t *sb, char c)
{
    if (sb->err)
        return;
    if (sb->len + 1 < sb->cap) {
        sb->buf[sb->len++] = c;
        sb->buf[sb->len] = 0;
    } else {
        sb->err = 1;
    }
}

void hl_sb_puts(sbuf_t *sb, const char *s)
{
    while (*s)
        hl_sb_putc(sb, *s++);
}

void hl_sb_printf(sbuf_t *sb, const char *fmt, ...)
{
    if (sb->err)
        return;
    va_list ap;
    va_start(ap, fmt);
    int room = (int)(sb->cap - sb->len);
    int n = vsnprintf(sb->buf + sb->len, (size_t)room, fmt, ap);
    va_end(ap);
    if (n < 0) {
        sb->err = 1;
        return;
    }
    if ((size_t)n >= (size_t)room) {
        sb->err = 1;
        return;
    }
    sb->len += (size_t)n;
}

char *hl_json_quote_dyn(const char *s)
{
    size_t cap = strlen(s) * 6 + 3;
    char *out = malloc(cap);
    if (!out)
        return NULL;
    sbuf_t sb;
    hl_sb_init(&sb, out, cap);
    hl_sb_putc(&sb, '"');
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        switch (*p) {
        case '"':  hl_sb_puts(&sb, "\\\""); break;
        case '\\': hl_sb_puts(&sb, "\\\\"); break;
        case '\n': hl_sb_puts(&sb, "\\n");  break;
        case '\r': hl_sb_puts(&sb, "\\r");  break;
        case '\t': hl_sb_puts(&sb, "\\t");  break;
        case '\b': hl_sb_puts(&sb, "\\b");  break;
        case '\f': hl_sb_puts(&sb, "\\f");  break;
        default:
            if (*p < 0x20)
                hl_sb_printf(&sb, "\\u%04x", *p);
            else
                hl_sb_putc(&sb, (char)*p); /* raw UTF-8 passthrough */
        }
    }
    hl_sb_putc(&sb, '"');
    if (sb.err) {
        free(out);
        return NULL;
    }
    return out;
}

void hl_sb_json_str(sbuf_t *sb, const char *s)
{
    char *q = hl_json_quote_dyn(s ? s : "");
    if (!q) {
        sb->err = 1;
        return;
    }
    hl_sb_puts(sb, q);
    free(q);
}
