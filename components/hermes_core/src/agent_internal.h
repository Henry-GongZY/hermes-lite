#ifndef HL_AGENT_INTERNAL_H
#define HL_AGENT_INTERNAL_H

#include "hermes_lite.h"
#include "json.h"
#include "skills.h"

typedef struct hl_msg {
    char *role;         /* "user" | "assistant" | "tool" */
    char *content;      /* may be NULL */
    char *tool_calls;   /* assistant: rebuilt raw JSON array, else NULL */
    char *tool_call_id; /* role "tool" */
    struct hl_msg *next;
} hl_msg_t;

struct hl_agent {
    hl_config_t cfg;
    const hl_platform_t *plat;
    hl_tool_t *tools;
    size_t n_tools;
    hl_msg_t *head, *tail;

    char *req, *resp, *arena, *hist_buf, *tool_out, *skill_buf;
    size_t req_cap, resp_cap, arena_cap, hist_cap, tool_out_cap, skill_cap;
    size_t hist_size; /* current history file size (tracked for compaction) */
};

hl_msg_t *hl_msg_new(hl_agent_t *a, const char *role, const char *content,
                     const char *tool_calls, const char *tool_call_id);

/* Serialize one message as a JSON object (no trailing newline). */
void hl_msg_emit(sbuf_t *sb, const hl_msg_t *m);

/* JSONL persistence; no-ops when cfg.history_path is unset. */
void hl_history_append(hl_agent_t *a, const hl_msg_t *m);
void hl_history_load(hl_agent_t *a);
/* Rewrite the history file keeping only a recent tail (wear/bound control). */
void hl_history_compact(hl_agent_t *a, size_t keep_chars);

#endif
