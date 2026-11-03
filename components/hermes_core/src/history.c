#include "agent_internal.h"
#include "json.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* Conversation persistence: one JSON object per line (JSONL), reusing the
 * same shape as the wire format so replay is trivial. The file is compacted
 * in place once it outgrows its load scratch — with a best-effort LLM
 * summary of the dropped tail written as the thread opener, so long
 * conversations keep their context instead of losing it. */

static size_t hmsg_chars(const hl_msg_t *m)
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

void hl_history_append(hl_agent_t *a, const hl_msg_t *m)
{
    if (!a->cfg.history_path || !a->plat->fs_append)
        return;
    /* a->req is free between builds; reuse it as the line scratch */
    sbuf_t sb;
    hl_sb_init(&sb, a->req, a->req_cap);
    hl_msg_emit(&sb, m);
    hl_sb_putc(&sb, '\n');
    if (sb.err)
        return;
    if (a->plat->fs_append(a->cfg.history_path, sb.buf, sb.len) < 0)
        return;
    a->hist_size += sb.len;

    /* compact when the file outgrows 3/4 of its load scratch; keep at
     * most half the scratch so the next trigger needs real growth */
    if (a->hist_cap && a->hist_size > (a->hist_cap * 3) / 4) {
        size_t keep = a->cfg.max_ctx_chars ? a->cfg.max_ctx_chars / 2 : 8000;
        if (keep > a->hist_cap / 2)
            keep = a->hist_cap / 2;
        hl_history_compact(a, keep);
    }
}

void hl_history_load(hl_agent_t *a)
{
    if (!a->cfg.history_path || !a->plat->fs_read)
        return;
    size_t len = 0;
    if (a->plat->fs_read(a->cfg.history_path, a->hist_buf, a->hist_cap, &len) < 0)
        return; /* no history yet */
    a->hist_buf[len] = 0;
    a->hist_size = len;

    char *p = a->hist_buf;
    char *arena = a->arena;
    size_t apos = 0;
    while (p && *p) {
        char *nl = strchr(p, '\n');
        if (nl)
            *nl = 0;
        size_t used = 0;
        jval_t *v = hl_json_parse(arena + apos, a->arena_cap - apos, p, &used);
        if (v) {
            apos += used;
            jval_t *role = hl_jobj_get(v, "role");
            jval_t *content = hl_jobj_get(v, "content");
            jval_t *tcs = hl_jobj_get(v, "tool_calls");
            jval_t *tcid = hl_jobj_get(v, "tool_call_id");
            if (role && role->type == J_STR &&
                (!tcs || tcs->type == J_ARR)) {
                /* skip empty assistant lines: useless context that some
                 * upstreams answer with an empty body */
                if (!strcmp(role->str, "assistant") &&
                    (!content || !content->type == J_STR || !content->str[0]) &&
                    !tcs)
                    goto next_line;
                char *tcs_raw = NULL;
                if (tcs) {
                    /* serialize the parsed array back to raw wire text */
                    sbuf_t sb;
                    hl_sb_init(&sb, a->req, a->req_cap);
                    hl_sb_putc(&sb, '[');
                    int cnt = hl_jarr_len(tcs);
                    for (int i = 0; i < cnt; i++) {
                        jval_t *tc = hl_jarr_get(tcs, i);
                        if (i)
                            hl_sb_putc(&sb, ',');
                        hl_sb_puts(&sb, "{\"id\":");
                        jval_t *id = hl_jobj_get(tc, "id");
                        hl_sb_json_str(&sb, id && id->type == J_STR ? id->str : "");
                        hl_sb_puts(&sb, ",\"type\":\"function\",\"function\":{\"name\":");
                        jval_t *fn = hl_jobj_get(tc, "function");
                        jval_t *nm = fn ? hl_jobj_get(fn, "name") : NULL;
                        hl_sb_json_str(&sb, nm && nm->type == J_STR ? nm->str : "");
                        hl_sb_puts(&sb, ",\"arguments\":");
                        jval_t *ar = fn ? hl_jobj_get(fn, "arguments") : NULL;
                        hl_sb_json_str(&sb, ar && ar->type == J_STR ? ar->str : "{}");
                        hl_sb_puts(&sb, "}}");
                    }
                    hl_sb_putc(&sb, ']');
                    /* hl_msg_new copies synchronously; a->req reuse is safe */
                    tcs_raw = sb.err ? NULL : a->req;
                }
                hl_msg_new(a, role->str,
                           content && content->type == J_STR ? content->str : NULL,
                           tcs_raw,
                           tcid && tcid->type == J_STR ? tcid->str : NULL);
            }
        }
        next_line:
        /* on parse failure just skip the line (possibly truncated head) */
        p = nl ? nl + 1 : NULL;
    }
}

/* Ask the LLM for a summary of the messages that compaction is about to
 * drop. Returns a malloc'd summary text, or NULL (any failure -> caller
 * falls back to plain dropping). No retries: compaction is best-effort. */
static char *summarize_dropped(hl_agent_t *a, hl_msg_t **dropped, int n_dropped)
{
    if (!a->plat->http_post || n_dropped <= 0)
        return NULL;

    /* transcript of the dropped messages (bounded: newest 8 KB wins) */
    size_t cap = 8192;
    for (int i = 0; i < n_dropped; i++)
        cap += hmsg_chars(dropped[i]);
    char *transcript = malloc(cap);
    if (!transcript)
        return NULL;
    sbuf_t tb;
    hl_sb_init(&tb, transcript, cap);
    for (int i = 0; i < n_dropped; i++) {
        hl_msg_emit(&tb, dropped[i]);
        hl_sb_putc(&tb, '\n');
    }
    if (tb.err) {
        free(transcript);
        return NULL;
    }
    if (tb.len > 8000) { /* keep only the newest 8 KB */
        memmove(transcript, transcript + (tb.len - 8000), 8000);
        tb.len = 8000;
        transcript[tb.len] = 0;
    }

    char *q = hl_json_quote_dyn(transcript);
    free(transcript);
    if (!q)
        return NULL;

    char *body = malloc(strlen(q) + 1024);
    if (!body) {
        free(q);
        return NULL;
    }
    snprintf(body, strlen(q) + 1024,
             "{\"model\":\"%s\",\"stream\":false,\"max_tokens\":220,"
             "\"messages\":[{\"role\":\"system\",\"content\":"
             "\"Summarize the following conversation excerpt in under 150 words, "
             "preserving key facts, decisions, names, and open questions. "
             "Output only the summary.\"},{\"role\":\"user\",\"content\":%s}]}",
             a->cfg.model, q);
    free(q);

    char *resp = malloc(4096 + 1);
    if (!resp) {
        free(body);
        return NULL;
    }
    size_t rlen = 0;
    int st = a->plat->http_post(a->cfg.api_url, a->cfg.api_key, body, strlen(body),
                                resp, 4096, &rlen);
    free(body);
    char *out = NULL;
    if (st == 200) {
        resp[rlen] = 0;
        char *arena = malloc(4096 + 64);
        if (arena) {
            jval_t *root = hl_json_parse(arena, 4096 + 64, resp, NULL);
            jval_t *c0 = root ? hl_jarr_get(hl_jobj_get(root, "choices"), 0) : NULL;
            jval_t *msg = c0 ? hl_jobj_get(c0, "message") : NULL;
            jval_t *content = msg ? hl_jobj_get(msg, "content") : NULL;
            if (content && content->type == J_STR && content->str[0])
                out = strdup(content->str);
            free(arena);
        }
    }
    free(resp);
    return out;
}

void hl_history_compact(hl_agent_t *a, size_t keep_chars)
{
    int n = 0;
    for (hl_msg_t *m = a->head; m; m = m->next)
        n++;
    if (n == 0)
        return;
    hl_msg_t **arr = malloc((size_t)n * sizeof *arr);
    if (!arr)
        return;
    int i = 0;
    for (hl_msg_t *m = a->head; m; m = m->next)
        arr[i++] = m;

    /* keep the newest tail that fits keep_chars */
    size_t total = 0;
    int start = n;
    while (start > 0 && total + hmsg_chars(arr[start - 1]) <= keep_chars) {
        start--;
        total += hmsg_chars(arr[start]);
    }
    /* a replayable thread starts at a user message */
    while (start < n && strcmp(arr[start]->role, "user") != 0)
        start++;
    if (start >= n) {
        /* nothing fit: fall back to the final user turn */
        int last_user = n - 1;
        while (last_user >= 0 && strcmp(arr[last_user]->role, "user") != 0)
            last_user--;
        start = last_user;
    }
    if (start < 0) {
        free(arr);
        return;
    }

    /* best-effort summary of the dropped tail, written as the thread opener */
    char *summary = start > 0 ? summarize_dropped(a, arr, start) : NULL;

    sbuf_t sb;
    hl_sb_init(&sb, a->req, a->req_cap);
    if (summary) {
        char scontent[1400];
        snprintf(scontent, sizeof scontent,
                 "[Earlier conversation summary]\n%s", summary);
        hl_sb_puts(&sb, "{\"role\":\"user\",\"content\":");
        hl_sb_json_str(&sb, scontent);
        hl_sb_puts(&sb, "}\n");
    }
    for (int k = start; k < n; k++) {
        hl_msg_emit(&sb, arr[k]);
        hl_sb_putc(&sb, '\n');
    }
    if (sb.err) {
        free(arr);
        free(summary);
        return;
    }

    /* mutate the in-memory list to match the file: drop arr[0..start] */
    for (int k = 0; k < start && k < n; k++) {
        free(arr[k]->role);
        free(arr[k]->content);
        free(arr[k]->tool_calls);
        free(arr[k]->tool_call_id);
        free(arr[k]);
    }
    a->head = start < n ? arr[start] : NULL;
    if (!a->head)
        a->tail = NULL;
    free(arr);
    free(summary);
    if (a->plat->fs_write(a->cfg.history_path, sb.buf, sb.len) == 0)
        a->hist_size = sb.len;
}
