#include "hermes_lite.h"
#include "json.h"

#include <zephyr/kernel.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

/* Agent-loop self-test running under the Zephyr kernel on native_sim.
 * The HTTP layer is scripted (no network); exercises: request build,
 * streamed reply assembly, tool-call round trip, unknown-tool feedback,
 * iteration limit. Exit code 0 on success (visible via `west build -t run`
 * exit status or the PASS/FAIL line). */

#define MAX_STEPS 8

/* picolibc minimal config may hide strdup */
static char *xstrdup(const char *s)
{
    size_t n = strlen(s) + 1;
    char *r = malloc(n);
    if (r)
        memcpy(r, s, n);
    return r;
}

static char *captured[MAX_STEPS];
static int n_captured;
static int cur_step;

struct step {
    int stream;              /* 1: use http_post_stream, 0: http_post */
    const char *payload;     /* buffered JSON, or \n-separated SSE data lines */
};

static struct step steps[MAX_STEPS];
static int n_steps;

static char frag_log[2048];

static void mock_reset(void)
{
    for (int i = 0; i < n_captured; i++)
        free(captured[i]);
    n_captured = 0;
    cur_step = 0;
    n_steps = 0;
    frag_log[0] = 0;
}

static void step_add(int stream, const char *payload)
{
    steps[n_steps].stream = stream;
    steps[n_steps].payload = payload;
    n_steps++;
}

static int mock_post(const char *url, const char *auth, const char *body,
                     size_t bl, char *resp, size_t cap, size_t *rl)
{
    printk("[mock] buffered POST (%d chars)\n", (int)bl);
    if (n_captured < MAX_STEPS)
        captured[n_captured++] = xstrdup(body);
    if (cur_step >= n_steps || steps[cur_step].stream)
        return -1;
    const char *r = steps[cur_step++].payload;
    size_t n = strlen(r);
    if (n > cap)
        n = cap;
    memcpy(resp, r, n);
    *rl = n;
    return 200;
}

static int mock_stream(const char *url, const char *auth, const char *body,
                       size_t bl, hl_line_fn on_line, void *ud,
                       char *errbuf, size_t errcap)
{
    printk("[mock] streaming POST (%d chars)\n", (int)bl);
    if (n_captured < MAX_STEPS)
        captured[n_captured++] = xstrdup(body);
    if (cur_step >= n_steps || !steps[cur_step].stream)
        return -1;
    char *tmp = xstrdup(steps[cur_step++].payload);
    for (char *ln = strtok(tmp, "\n"); ln; ln = strtok(NULL, "\n"))
        if (on_line(ud, ln))
            break;
    free(tmp);
    return 200;
}

static void on_frag(void *ud, const char *frag)
{
    (void)ud;
    strcat(frag_log, frag);
}

static void on_line_log(void *ud, const char *data)
{
    (void)ud;
    printk("[sse] %.60s\n", data);
}

/* in-memory fs stub: no history/skills in this run */
static int fs_stub(const char *p, char *b, size_t c, size_t *l)
{
    (void)p; (void)b; (void)c; (void)l;
    return -1;
}
static int fs_stub2(const char *p, const char *d, size_t n)
{
    (void)p; (void)d; (void)n;
    return -1;
}
static int fs_stub3(const char *p, int (*cb)(void *, const char *), void *ud)
{
    (void)p; (void)cb; (void)ud;
    return -1;
}
static unsigned long tms(void)
{
    return (unsigned long)k_uptime_get();
}
static void nolog(int level, const char *msg)
{
    printk("[hl:%d] %.100s\n", level, msg);
}

/* tool: add {a,b} */
static int tool_add(void *ud, const char *args, char *out, size_t cap)
{
    (void)ud;
    char arena[512];
    jval_t *v = hl_json_parse(arena, sizeof arena, args, NULL);
    jval_t *a = hl_jobj_get(v, "a"), *b = hl_jobj_get(v, "b");
    if (!a || !b) {
        snprintf(out, cap, "error: need a and b");
        return 1;
    }
    snprintf(out, cap, "%d", (int)(a->num + b->num));
    return 0;
}

static int fails;

#define CHECK(cond, msg)                                                       \
    do {                                                                       \
        if (cond) {                                                            \
            printk("PASS: %s\n", msg);                                         \
        } else {                                                               \
            printk("FAIL: %s\n", msg);                                         \
            fails++;                                                           \
        }                                                                      \
    } while (0)

int main(void)
{
    printk("=== hermes-core on Zephyr (%s) ===\n", CONFIG_BOARD_TARGET);

    static const hl_platform_t mock = {
        .http_post = mock_post,
        .http_post_stream = mock_stream,
        .fs_read = fs_stub,
        .fs_write = fs_stub2,
        .fs_append = fs_stub2,
        .fs_list = fs_stub3,
        .time_ms = tms,
        .log = nolog,
    };

    hl_config_t cfg = {
        .api_url = "https://mock.invalid/v1/chat/completions",
        .api_key = "test",
        .model = "test-model",
        .system_prompt = "selftest",
        .max_ctx_chars = 16000,
        .buf_req = 24576,
        .buf_resp = 24576,
        .buf_arena = 24576,
        .buf_hist = 8192,
        .buf_skill = 8192,
    };
    hl_tool_t tools[] = {
        { .name = "add", .description = "add two ints",
          .parameters_json = "{\"type\":\"object\"}", .fn = tool_add },
    };

    /* --- streamed reply, no tools --- */
    mock_reset();
    step_add(1,
             "{\"choices\":[{\"delta\":{\"content\":\"Hel\"}}]}\n"
             "{\"choices\":[{\"delta\":{\"content\":\"lo zephyr\"}}]}\n"
             "[DONE]");
    hl_agent_t *a = hl_agent_new(&cfg, &mock, NULL, 0);
    char reply[512];
    int r = hl_agent_chat_stream(a, "hi", on_frag, NULL, reply, sizeof reply);
    CHECK(r == 0 && !strcmp(reply, "Hello zephyr"), "streamed reply under Zephyr");
    CHECK(!strcmp(frag_log, "Hello zephyr"), "fragments live");
    CHECK(strstr(captured[0], "\"stream\":true"), "streaming request flagged");
    hl_agent_free(a);

    /* --- tool-call round trip --- */
    mock_reset();
    step_add(1,
             "{\"choices\":[{\"delta\":{\"tool_calls\":[{\"index\":0,\"id\":\"c1\","
             "\"function\":{\"name\":\"add\",\"arguments\":\"{\\\"a\\\":1,\\\"b\\\":2}\""
             "}}]}}]}\n[DONE]");
    step_add(1,
             "{\"choices\":[{\"delta\":{\"content\":\"sum is 3\"}}]}\n[DONE]");
    a = hl_agent_new(&cfg, &mock, tools, 1);
    r = hl_agent_chat_stream(a, "add 1 and 2", NULL, NULL, reply, sizeof reply);
    CHECK(r == 0 && !strcmp(reply, "sum is 3"), "tool round trip");
    CHECK(strstr(captured[1], "\"content\":\"3\""), "tool result fed back");
    hl_agent_free(a);

    /* --- iteration limit --- */
    mock_reset();
    for (int i = 0; i < 8; i++)
        step_add(1,
                 "{\"choices\":[{\"delta\":{\"tool_calls\":[{\"index\":0,\"id\":\"cx\","
                 "\"function\":{\"name\":\"add\",\"arguments\":\"{}\"}}]}}]}\n[DONE]");
    a = hl_agent_new(&cfg, &mock, tools, 1);
    cfg.max_tool_iters = 3;
    r = hl_agent_chat_stream(a, "loop", NULL, NULL, reply, sizeof reply);
    CHECK(r < 0 && strstr(reply, "iteration limit"), "iteration limit enforced");
    hl_agent_free(a);

    if (fails) {
        printk("=== %d TEST(S) FAILED ===\n", fails);
        return 1;
    }
    printk("=== ALL ZEPHYR TESTS PASSED ===\n");
    return 0;
}
