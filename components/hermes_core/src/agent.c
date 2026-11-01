#include "hermes_lite.h"
#include "agent_internal.h"
#include "json.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>

#define HL_DEF_MAX_ITERS 6
#define HL_DEF_CTX_CHARS 16000
#define HL_MAX_STREAM_CALLS 8
#define HL_HTTP_ATTEMPTS 3 /* 1 initial + 2 retries on transport/429/5xx */

static void alog(hl_agent_t *a, int lvl, const char *fmt, ...)
{
    if (!a->plat->log)
        return;
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    a->plat->log(lvl, buf);
}

static char *xstrdup(const char *s)
{
    size_t n = strlen(s) + 1;
    char *r = malloc(n);
    if (r)
        memcpy(r, s, n);
    return r;
}

/* growable string builder (request/message assembly that must not cap) */
typedef struct {
    char *buf;
    size_t len, cap;
    int err;
} dsb_t;

static void dsb_put(dsb_t *d, const char *s, size_t n)
{
    if (d->err)
        return;
    if (d->len + n + 1 > d->cap) {
        size_t nc = d->cap ? d->cap : 256;
        while (d->len + n + 1 > nc)
            nc *= 2;
        char *nb = realloc(d->buf, nc);
        if (!nb) {
            d->err = 1;
            return;
        }
        d->buf = nb;
        d->cap = nc;
    }
    memcpy(d->buf + d->len, s, n);
    d->len += n;
    d->buf[d->len] = 0;
}

static void dsb_puts(dsb_t *d, const char *s)
{
    dsb_put(d, s, strlen(s));
}

static void dsb_json(dsb_t *d, const char *s)
{
    char *q = hl_json_quote_dyn(s);
    if (!q) {
        d->err = 1;
        return;
    }
    dsb_puts(d, q);
    free(q);
}

hl_msg_t *hl_msg_new(hl_agent_t *a, const char *role, const char *content,
                     const char *tool_calls, const char *tool_call_id)
{
    hl_msg_t *m = calloc(1, sizeof *m);
    if (!m)
        return NULL;
    m->role = xstrdup(role);
    m->content = content ? xstrdup(content) : NULL;
    m->tool_calls = tool_calls ? xstrdup(tool_calls) : NULL;
    m->tool_call_id = tool_call_id ? xstrdup(tool_call_id) : NULL;
    if (a->tail)
        a->tail->next = m;
    else
        a->head = m;
    a->tail = m;
    return m;
}

void hl_msg_emit(sbuf_t *sb, const hl_msg_t *m)
{
    if (!strcmp(m->role, "assistant") && m->tool_calls) {
        hl_sb_puts(sb, "{\"role\":\"assistant\",\"content\":");
        if (m->content && m->content[0])
            hl_sb_json_str(sb, m->content);
        else
            hl_sb_puts(sb, "null");
        hl_sb_puts(sb, ",\"tool_calls\":");
        hl_sb_puts(sb, m->tool_calls);
        hl_sb_putc(sb, '}');
    } else if (!strcmp(m->role, "tool")) {
        hl_sb_puts(sb, "{\"role\":\"tool\",\"tool_call_id\":");
        hl_sb_json_str(sb, m->tool_call_id ? m->tool_call_id : "");
        hl_sb_puts(sb, ",\"content\":");
        hl_sb_json_str(sb, m->content ? m->content : "");
        hl_sb_putc(sb, '}');
    } else {
        hl_sb_puts(sb, "{\"role\":\"");
        hl_sb_puts(sb, m->role);
        hl_sb_puts(sb, "\",\"content\":");
        hl_sb_json_str(sb, m->content ? m->content : "");
        hl_sb_putc(sb, '}');
    }
}

/* ---------------- skills built-in tool ---------------- */

static int tool_read_skill(void *ud, const char *args, char *out, size_t cap)
{
    hl_agent_t *a = ud;
    char arena[512];
    jval_t *v = hl_json_parse(arena, sizeof arena, args && *args ? args : "{}", NULL);
    jval_t *n = hl_jobj_get(v, "name");
    if (!n || n->type != J_STR || !n->str[0]) {
        snprintf(out, cap, "error: missing skill name");
        return 1;
    }
    if (hl_skills_load(a->plat, a->cfg.skills_dir, n->str, out, cap - 1) < 0) {
        snprintf(out, cap, "error: skill '%s' not found", n->str);
        return 1;
    }
    return 0;
}

/* ---------------- request building ---------------- */

static hl_tool_t *find_tool(hl_agent_t *a, const char *name)
{
    for (size_t i = 0; i < a->n_tools; i++)
        if (!strcmp(a->tools[i].name, name))
            return &a->tools[i];
    return NULL;
}

static size_t msg_chars(const hl_msg_t *m)
{
    size_t n = strlen(m->role) + 16;
    if (m->content)
        n += strlen(m->content);
    if (m->tool_calls)
        n += strlen(m->tool_calls);
    if (m->tool_call_id)
        n += strlen(m->tool_call_id);
    return n;
}

/* First message to send: drop oldest until under budget, and never start
 * with a dangling tool/assistant-tool_calls block (those only follow a user
 * message, so rewinding to the first user message keeps the thread valid). */
static hl_msg_t *trim_start(hl_agent_t *a)
{
    size_t budget = a->cfg.max_ctx_chars ? a->cfg.max_ctx_chars : HL_DEF_CTX_CHARS;
    size_t total = 0;
    for (hl_msg_t *m = a->head; m; m = m->next)
        total += msg_chars(m);
    hl_msg_t *start = a->head;
    while (start && total > budget) {
        total -= msg_chars(start);
        start = start->next;
    }
    while (start && strcmp(start->role, "user") != 0)
        start = start->next;
    return start ? start : a->tail;
}

static int build_request(hl_agent_t *a, sbuf_t *sb, int stream)
{
    /* system prompt + skills index (scratch: skill_buf; index scan uses
     * tool_out as file scratch, which is free until the tool phase) */
    sbuf_t sys;
    hl_sb_init(&sys, a->skill_buf, a->skill_cap);
    hl_sb_puts(&sys, a->cfg.system_prompt ? a->cfg.system_prompt
                                          : "You are Hermes Lite, a helpful assistant.");
    if (a->cfg.skills_dir) {
        hl_sb_puts(&sys, "\n\n# Skills\n"
                         "Before following instructions from a skill, load it with the "
                         "read_skill tool. Available skills:");
        hl_skills_build_index(a->plat, a->cfg.skills_dir, &sys, a->tool_out, a->tool_out_cap);
    }
    if (sys.err)
        return -1;

    hl_sb_puts(sb, "{\"model\":\"");
    hl_sb_puts(sb, a->cfg.model);
    hl_sb_printf(sb, "\",\"stream\":%s,\"messages\":[", stream ? "true" : "false");
    hl_sb_puts(sb, "{\"role\":\"system\",\"content\":");
    hl_sb_json_str(sb, sys.buf);
    hl_sb_putc(sb, '}');

    hl_msg_t *start = trim_start(a);
    for (hl_msg_t *m = start; m; m = m->next) {
        hl_sb_putc(sb, ',');
        hl_msg_emit(sb, m);
    }

    hl_sb_puts(sb, "]");
    if (a->n_tools) {
        hl_sb_puts(sb, ",\"tools\":[");
        for (size_t i = 0; i < a->n_tools; i++) {
            hl_tool_t *t = &a->tools[i];
            if (i)
                hl_sb_putc(sb, ',');
            hl_sb_puts(sb, "{\"type\":\"function\",\"function\":{\"name\":");
            hl_sb_json_str(sb, t->name);
            hl_sb_puts(sb, ",\"description\":");
            hl_sb_json_str(sb, t->description ? t->description : "");
            hl_sb_puts(sb, ",\"parameters\":");
            hl_sb_puts(sb, t->parameters_json ? t->parameters_json : "{}");
            hl_sb_puts(sb, "}}");
        }
        hl_sb_putc(sb, ']');
    }
    hl_sb_putc(sb, '}');
    return sb->err ? -1 : 0;
}

/* ---------------- tool phase (shared by buffered and streaming) -------- */

typedef struct {
    const char *id, *name, *args;
} tc_ref_t;

static void exec_and_record_tools(hl_agent_t *a, const tc_ref_t *tcs, int n)
{
    for (int i = 0; i < n; i++) {
        const char *tname = tcs[i].name;
        const char *targs = tcs[i].args;
        const char *tid = tcs[i].id;

        alog(a, HL_LOG_INFO, "tool %s(%s)", tname, targs);
        a->tool_out[0] = 0;
        hl_tool_t *t = find_tool(a, tname);
        int rc = t ? t->fn(t->ud, targs, a->tool_out, a->tool_out_cap) : 2;
        if (!t)
            snprintf(a->tool_out, a->tool_out_cap, "error: unknown tool '%s'", tname);
        else if (rc != 0 && !a->tool_out[0])
            snprintf(a->tool_out, a->tool_out_cap, "error: tool '%s' failed (%d)",
                     tname, rc);
        a->tool_out[a->tool_out_cap - 1] = 0;
        alog(a, HL_LOG_INFO, "-> %.120s", a->tool_out);
        hl_msg_new(a, "tool", a->tool_out, NULL, tid);
        hl_history_append(a, a->tail);
    }
}

/* materialize an OpenAI tool_calls array from id/name/args strings */
static char *tool_calls_json(int n, const tc_ref_t *tcs)
{
    dsb_t d = { 0 };
    dsb_puts(&d, "[");
    for (int i = 0; i < n; i++) {
        if (i)
            dsb_puts(&d, ",");
        dsb_puts(&d, "{\"id\":");
        dsb_json(&d, tcs[i].id ? tcs[i].id : "");
        dsb_puts(&d, ",\"type\":\"function\",\"function\":{\"name\":");
        dsb_json(&d, tcs[i].name ? tcs[i].name : "");
        dsb_puts(&d, ",\"arguments\":");
        dsb_json(&d, tcs[i].args ? tcs[i].args : "{}");
        dsb_puts(&d, "}}");
    }
    dsb_puts(&d, "]");
    if (d.err) {
        free(d.buf);
        return NULL;
    }
    return d.buf ? d.buf : xstrdup("[]");
}

/* buffered path: extract tc_refs from the parsed response */
static int refs_from_parsed(jval_t *tcs, tc_ref_t *refs)
{
    int n = hl_jarr_len(tcs);
    if (n > HL_MAX_STREAM_CALLS)
        n = HL_MAX_STREAM_CALLS;
    for (int i = 0; i < n; i++) {
        jval_t *tc = hl_jarr_get(tcs, i);
        jval_t *id = hl_jobj_get(tc, "id");
        jval_t *fn = hl_jobj_get(tc, "function");
        jval_t *nm = fn ? hl_jobj_get(fn, "name") : NULL;
        jval_t *ar = fn ? hl_jobj_get(fn, "arguments") : NULL;
        refs[i].id = (id && id->type == J_STR) ? id->str : "";
        refs[i].name = (nm && nm->type == J_STR) ? nm->str : "";
        refs[i].args = (ar && ar->type == J_STR && ar->str[0]) ? ar->str : "{}";
    }
    return n;
}

/* ---------------- streaming accumulator ---------------- */

typedef struct {
    dsb_t id, name, args;
    int used;
} tc_slot_t;

typedef struct {
    hl_agent_t *a;
    dsb_t content;
    tc_slot_t slots[HL_MAX_STREAM_CALLS];
    int n_slots;
    hl_text_fn on_text;
    void *ud;
    int err;
} sacc_t;

static int sse_on_line(void *vacc, const char *data)
{
    sacc_t *s = vacc;
    hl_agent_t *a = s->a;
    if (!strcmp(data, "[DONE]"))
        return 0;
    jval_t *v = hl_json_parse(a->arena, a->arena_cap, data, NULL);
    if (!v)
        return 0; /* keepalives, comments, partial junk: ignore */
    jval_t *d0 = hl_jarr_get(hl_jobj_get(v, "choices"), 0);
    jval_t *delta = d0 ? hl_jobj_get(d0, "delta") : NULL;
    if (!delta)
        return 0;

    jval_t *c = hl_jobj_get(delta, "content");
    if (c && c->type == J_STR && c->str[0]) {
        dsb_puts(&s->content, c->str);
        if (s->on_text)
            s->on_text(s->ud, c->str);
    }
    jval_t *tcs = hl_jobj_get(delta, "tool_calls");
    if (tcs && tcs->type == J_ARR) {
        for (jval_t *tc = tcs->child; tc; tc = tc->next) {
            jval_t *idx = hl_jobj_get(tc, "index");
            int i = (idx && idx->type == J_NUM) ? (int)idx->num : 0;
            if (i < 0 || i >= HL_MAX_STREAM_CALLS)
                continue;
            tc_slot_t *slot = &s->slots[i];
            if (!slot->used) {
                slot->used = 1;
                if (i + 1 > s->n_slots)
                    s->n_slots = i + 1;
            }
            jval_t *id = hl_jobj_get(tc, "id");
            if (id && id->type == J_STR)
                dsb_puts(&slot->id, id->str);
            jval_t *fn = hl_jobj_get(tc, "function");
            if (fn) {
                jval_t *nm = hl_jobj_get(fn, "name");
                if (nm && nm->type == J_STR)
                    dsb_puts(&slot->name, nm->str);
                jval_t *ar = hl_jobj_get(fn, "arguments");
                if (ar && ar->type == J_STR && ar->str[0])
                    dsb_puts(&slot->args, ar->str);
            }
        }
    }
    if (s->content.err)
        s->err = 1;
    return 0;
}

static void sacc_free(sacc_t *s)
{
    free(s->content.buf);
    for (int i = 0; i < HL_MAX_STREAM_CALLS; i++) {
        free(s->slots[i].id.buf);
        free(s->slots[i].name.buf);
        free(s->slots[i].args.buf);
    }
}

/* ---------------- the loop ---------------- */

static int agent_turn(hl_agent_t *a, const char *user_msg, hl_text_fn on_text,
                      void *tud, char *reply, size_t reply_cap, int prefer_stream)
{
    if (reply_cap)
        reply[0] = 0;
    if (!hl_msg_new(a, "user", user_msg, NULL, NULL))
        return -1;
    hl_history_append(a, a->tail);

    int iters = a->cfg.max_tool_iters > 0 ? a->cfg.max_tool_iters : HL_DEF_MAX_ITERS;

    for (int it = 0; it < iters; it++) {
        int stream = prefer_stream && a->plat->http_post_stream != NULL;
        sbuf_t sb;
        hl_sb_init(&sb, a->req, a->req_cap);
        if (build_request(a, &sb, stream) != 0) {
            alog(a, HL_LOG_ERROR, "request too large for buffer (%zu)", a->req_cap);
            return -1;
        }
        if (a->plat->log)
            a->plat->log(HL_LOG_DEBUG, sb.buf);

        const char *content = NULL;
        tc_ref_t refs[HL_MAX_STREAM_CALLS] = { 0 };
        int ntc = 0;
        sacc_t acc = { 0 };

        if (stream) {
            /* a->arena doubles as the per-line parse arena; a->resp doubles
             * as the non-200 error-body buffer */
            acc.a = a;
            acc.on_text = on_text;
            acc.ud = tud;
            int st = 0;
            for (int attempt = 0;; attempt++) {
                if (attempt > 0) {
                    /* fresh accumulator: a retried stream must not double-
                     * deliver fragments (only retried when nothing arrived) */
                    sacc_free(&acc);
                    memset(&acc, 0, sizeof acc);
                    acc.a = a;
                    acc.on_text = on_text;
                    acc.ud = tud;
                }
                st = a->plat->http_post_stream(a->cfg.api_url, a->cfg.api_key,
                                               a->req, sb.len, sse_on_line, &acc,
                                               a->resp, a->resp_cap);
                int has_data = acc.content.len > 0 || acc.n_slots > 0;
                int retryable = ((st < 0 && st != -2) || st == 429 ||
                                 (st >= 500 && st <= 599)) && !has_data;
                if (!retryable || attempt >= HL_HTTP_ATTEMPTS - 1)
                    break;
                alog(a, HL_LOG_INFO, "http stream attempt %d failed (%d); backing off",
                     attempt + 1, st);
                if (a->plat->delay_ms)
                    a->plat->delay_ms(500ul << attempt);
            }
            if (st < 0) {
                sacc_free(&acc);
                alog(a, HL_LOG_ERROR, "http stream transport error (%d)", st);
                return -1;
            }
            if (st != 200) {
                a->resp[a->resp_cap - 1] = 0;
                alog(a, HL_LOG_ERROR, "http %d: %.200s", st, a->resp);
                sacc_free(&acc);
                return -1;
            }
            if (acc.err) {
                sacc_free(&acc);
                alog(a, HL_LOG_ERROR, "stream too large for memory");
                return -1;
            }
            content = acc.content.buf;
            for (int i = 0; i < acc.n_slots; i++) {
                refs[i].id = acc.slots[i].id.buf;
                refs[i].name = acc.slots[i].name.buf;
                refs[i].args = acc.slots[i].args.buf;
            }
            ntc = acc.n_slots;
        } else {
            size_t rl = 0;
            int st = 0;
            for (int attempt = 0;; attempt++) {
                rl = 0;
                st = a->plat->http_post(a->cfg.api_url, a->cfg.api_key, a->req,
                                        sb.len, a->resp, a->resp_cap, &rl);
                int retryable = (st < 0 && st != -2) || st == 429 ||
                                (st >= 500 && st <= 599);
                if (!retryable || attempt >= HL_HTTP_ATTEMPTS - 1)
                    break;
                alog(a, HL_LOG_INFO, "http attempt %d failed (%d); backing off",
                     attempt + 1, st);
                if (a->plat->delay_ms)
                    a->plat->delay_ms(500ul << attempt);
            }
            if (st < 0) {
                alog(a, HL_LOG_ERROR, "http transport error (%d)", st);
                return -1;
            }
            if (st != 200) {
                a->resp[rl < a->resp_cap ? rl : a->resp_cap - 1] = 0;
                alog(a, HL_LOG_ERROR, "http %d: %.200s", st, a->resp);
                return -1;
            }
            a->resp[rl] = 0;

            size_t used = 0;
            jval_t *root = hl_json_parse(a->arena, a->arena_cap, a->resp, &used);
            if (!root) {
                alog(a, HL_LOG_ERROR, "response json parse failed");
                return -1;
            }
            jval_t *c0 = hl_jarr_get(hl_jobj_get(root, "choices"), 0);
            jval_t *msg = c0 ? hl_jobj_get(c0, "message") : NULL;
            if (!msg) {
                alog(a, HL_LOG_ERROR, "no choices[0].message in response");
                return -1;
            }
            jval_t *c = hl_jobj_get(msg, "content");
            jval_t *tcs = hl_jobj_get(msg, "tool_calls");
            content = (c && c->type == J_STR) ? c->str : "";
            ntc = tcs ? refs_from_parsed(tcs, refs) : 0;
        }

        char *tc_raw = ntc ? tool_calls_json(ntc, refs) : NULL;
        if (ntc && !tc_raw) {
            if (stream)
                sacc_free(&acc);
            return -1;
        }
        hl_msg_new(a, "assistant", (content && content[0]) ? content : NULL,
                   tc_raw, NULL);
        hl_history_append(a, a->tail);

        if (ntc == 0) {
            snprintf(reply, reply_cap, "%s", content ? content : "");
            if (stream)
                sacc_free(&acc);
            free(tc_raw);
            /* buffered fallback only: deliver the whole message as one
             * fragment (streaming already delivered fragments live) */
            if (on_text && !stream)
                on_text(tud, reply);
            return 0;
        }
        exec_and_record_tools(a, refs, ntc);
        if (stream)
            sacc_free(&acc);
        free(tc_raw);
    }

    snprintf(reply, reply_cap, "error: tool iteration limit reached");
    return -1;
}

int hl_agent_chat(hl_agent_t *a, const char *user_msg, char *reply, size_t reply_cap)
{
    return agent_turn(a, user_msg, NULL, NULL, reply, reply_cap, 0);
}

int hl_agent_chat_stream(hl_agent_t *a, const char *user_msg, hl_text_fn on_text,
                         void *ud, char *reply, size_t reply_cap)
{
    return agent_turn(a, user_msg, on_text, ud, reply, reply_cap, 1);
}

/* ---------------- lifecycle ---------------- */

hl_agent_t *hl_agent_new(const hl_config_t *cfg, const hl_platform_t *plat,
                         const hl_tool_t *tools, size_t n_tools)
{
    hl_agent_t *a = calloc(1, sizeof *a);
    if (!a)
        return NULL;
    a->cfg = *cfg;
    a->plat = plat;

    a->req_cap = cfg->buf_req ? cfg->buf_req : 65536;
    a->resp_cap = cfg->buf_resp ? cfg->buf_resp : 65536;
    a->arena_cap = cfg->buf_arena ? cfg->buf_arena : 65536;
    a->hist_cap = cfg->buf_hist ? cfg->buf_hist : 32768;
    a->skill_cap = cfg->buf_skill ? cfg->buf_skill : 8192;
    a->tool_out_cap = 4096;

    a->req = malloc(a->req_cap);
    a->resp = malloc(a->resp_cap + 1); /* +1 for parser NUL */
    a->arena = malloc(a->arena_cap);
    a->hist_buf = malloc(a->hist_cap + 1);
    a->skill_buf = malloc(a->skill_cap);
    a->tool_out = malloc(a->tool_out_cap);
    if (!a->req || !a->resp || !a->arena || !a->hist_buf || !a->skill_buf ||
        !a->tool_out) {
        hl_agent_free(a);
        return NULL;
    }

    /* copy tool table; append built-in read_skill when skills are enabled */
    size_t total = n_tools + (cfg->skills_dir ? 1 : 0);
    a->tools = malloc(total * sizeof(hl_tool_t));
    if (!a->tools) {
        hl_agent_free(a);
        return NULL;
    }
    if (n_tools)
        memcpy(a->tools, tools, n_tools * sizeof(hl_tool_t));
    a->n_tools = n_tools;
    if (cfg->skills_dir) {
        a->tools[a->n_tools++] = (hl_tool_t){
            .name = "read_skill",
            .description = "Load the full instructions of a skill by name. "
                           "Call this before acting on any skill.",
            .parameters_json =
                "{\"type\":\"object\",\"properties\":{\"name\":{\"type\":\"string\","
                "\"description\":\"skill name\"}},\"required\":[\"name\"]}",
            .fn = tool_read_skill,
            .ud = a,
        };
    }

    hl_history_load(a);
    return a;
}

void hl_agent_free(hl_agent_t *a)
{
    if (!a)
        return;
    hl_msg_t *m = a->head;
    while (m) {
        hl_msg_t *next = m->next;
        free(m->role);
        free(m->content);
        free(m->tool_calls);
        free(m->tool_call_id);
        free(m);
        m = next;
    }
    free(a->tools);
    free(a->req);
    free(a->resp);
    free(a->arena);
    free(a->hist_buf);
    free(a->skill_buf);
    free(a->tool_out);
    free(a);
}
