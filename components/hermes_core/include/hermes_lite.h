#ifndef HERMES_LITE_H
#define HERMES_LITE_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* hermes-lite: a minimal, portable agent runtime.
 *
 * The core (this API) is platform-independent C. A port supplies one
 * hl_platform_t with HTTP+TLS and filesystem primitives. The agent loop
 * talks to any OpenAI-compatible /v1/chat/completions endpoint and executes
 * locally registered tools; skills are markdown files (SKILL.md with
 * name/description frontmatter, agentskills.io compatible). */

#define HL_LOG_ERROR 0
#define HL_LOG_INFO  1
#define HL_LOG_DEBUG 2

/* SSE line callback: return 0 to continue, nonzero to abort the transfer. */
typedef int (*hl_line_fn)(void *ud, const char *data);

typedef struct hl_platform {
    /* Buffered HTTPS POST, Content-Type: application/json.
     * Returns the HTTP status code (e.g. 200); < 0 on transport failure.
     * On success *resp_len holds the response byte count. */
    int (*http_post)(const char *url, const char *auth_bearer,
                     const char *body, size_t body_len,
                     char *resp, size_t resp_cap, size_t *resp_len);

    /* Optional streaming POST: the SSE "data:" payload of every complete
     * line is passed to on_line (without the "data:" prefix, including the
     * literal "[DONE]"). on_line returning nonzero aborts the transfer
     * (reported as -3). When the HTTP status is not 2xx, the raw body goes
     * to errbuf instead and no lines are delivered. NULL = no streaming. */
    int (*http_post_stream)(const char *url, const char *auth_bearer,
                            const char *body, size_t body_len,
                            hl_line_fn on_line, void *ud,
                            char *errbuf, size_t errcap);

    /* Optional buffered HTTPS GET (used by OTA skill sync). NULL = no GET;
     * the agent's sync feature reports "not supported". */
    int (*http_get)(const char *url, const char *auth_bearer,
                    char *resp, size_t resp_cap, size_t *resp_len);

    /* Filesystem; 0 on success, negative on error.
     * fs_read writes at most cap bytes; caller must size buf cap+1 for NUL. */
    int (*fs_read)(const char *path, char *buf, size_t cap, size_t *len);
    int (*fs_write)(const char *path, const char *data, size_t len); /* truncate+write */
    int (*fs_append)(const char *path, const char *data, size_t len);
    /* Invoke cb(name) for each directory entry; nonzero cb return stops. */
    int (*fs_list)(const char *dir, int (*cb)(void *ud, const char *name), void *ud);

    unsigned long (*time_ms)(void);
    /* Sleep for backoff between HTTP retries; may be NULL (no wait). */
    void (*delay_ms)(unsigned long ms);
    void (*log)(int level, const char *msg);
} hl_platform_t;

typedef struct hl_config {
    const char *api_url;        /* "https://host/v1/chat/completions" */
    const char *api_key;        /* bearer token */
    const char *model;
    const char *system_prompt;  /* optional; prepended as system message */
    const char *skills_dir;     /* optional; enables skills + built-in read_skill */
    const char *history_path;   /* optional; JSONL conversation persistence.
                                   Borrowed: the caller-owned string must
                                   outlive the agent (not copied). */
    size_t max_ctx_chars;       /* per-request message char budget; 0 = 16000 */
    int    max_tool_iters;      /* tool rounds per turn; 0 = 6 */
    size_t buf_req;             /* request build buffer; 0 = 64KB */
    size_t buf_resp;            /* HTTP response buffer; 0 = 64KB */
    size_t buf_arena;           /* JSON parse arena; 0 = 64KB */
    size_t buf_hist;            /* history load scratch; 0 = 32KB */
    size_t buf_skill;           /* skill index/load scratch; 0 = 8KB */
} hl_config_t;

typedef int (*hl_tool_fn)(void *ud, const char *args_json, char *out, size_t out_cap);

typedef struct hl_tool {
    const char *name;
    const char *description;
    const char *parameters_json; /* JSON Schema, embedded verbatim; NULL = {} */
    hl_tool_fn  fn;
    void       *ud;
} hl_tool_t;

typedef struct hl_agent hl_agent_t;

/* cfg strings are borrowed (must outlive the agent). tools is copied;
 * if cfg.skills_dir is set, a built-in read_skill tool is appended. */
hl_agent_t *hl_agent_new(const hl_config_t *cfg, const hl_platform_t *plat,
                         const hl_tool_t *tools, size_t n_tools);
void hl_agent_free(hl_agent_t *a);

/* One user turn. Runs the tool loop until a final assistant message.
 * reply receives the final text (NUL-terminated). 0 on success, -1 on error. */
int hl_agent_chat(hl_agent_t *a, const char *user_msg, char *reply, size_t reply_cap);

/* Called for every streamed content fragment as it arrives. */
typedef void (*hl_text_fn)(void *ud, const char *fragment);

/* One user turn with SSE streaming. on_text (optional) sees content
 * fragments live; reply still receives the complete final text. Falls back
 * to buffered mode when the platform has no http_post_stream (on_text then
 * fires once with the whole message). 0 on success, -1 on error. */
int hl_agent_chat_stream(hl_agent_t *a, const char *user_msg,
                         hl_text_fn on_text, void *ud,
                         char *reply, size_t reply_cap);

/* Convenience for ports: returns the platform instance from that backend. */
const hl_platform_t *hl_platform_posix(void);
const hl_platform_t *hl_platform_esp32(void);
const hl_platform_t *hl_platform_zephyr(void); /* when built as Zephyr module */

#ifdef __cplusplus
}
#endif
#endif
