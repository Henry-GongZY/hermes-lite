#include "agent_internal.h"
#include "json.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* Conversation persistence: one JSON object per line (JSONL), reusing the
 * same shape as the wire format so replay is trivial. The file is compacted
 * in place once it outgrows its load scratch — keeps flash usage and wear
 * bounded on MCUs. */

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

    sbuf_t sb;
    hl_sb_init(&sb, a->req, a->req_cap);
    for (int k = start; k < n; k++) {
        hl_msg_emit(&sb, arr[k]);
        hl_sb_putc(&sb, '\n');
    }
    free(arr);
    if (sb.err)
        return;
    if (a->plat->fs_write(a->cfg.history_path, sb.buf, sb.len) == 0)
        a->hist_size = sb.len;
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

    /* compact when the file outgrows 3/4 of its load scratch; keep at most
     * half the scratch so the next trigger needs real growth to re-fire */
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
                /* tool_calls/tool_call_id are re-emitted by hl_msg_emit from
                 * the raw strings; for loaded history keep the JSON text */
                char *tcs_raw = NULL;
                if (tcs) {
                    /* serialize the parsed array back to raw text */
                    sbuf_t sb;
                    hl_sb_init(&sb, a->req, a->req_cap);
                    hl_sb_putc(&sb, '[');
                    int n = hl_jarr_len(tcs);
                    for (int i = 0; i < n; i++) {
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
                    tcs_raw = sb.err ? NULL : a->req; /* note: reused buffer */
                }
                hl_msg_new(a, role->str,
                           content && content->type == J_STR ? content->str : NULL,
                           tcs_raw,
                           tcid && tcid->type == J_STR ? tcid->str : NULL);
                /* tcs_raw pointed into a->req; hl_msg_new copied it already */
            }
        }
        /* on parse failure just skip the line (possibly truncated head) */
        p = nl ? nl + 1 : NULL;
    }
}
