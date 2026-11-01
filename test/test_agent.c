#include "hermes_lite.h"
#include "json.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <errno.h>
#include <sys/stat.h>
#include <unistd.h>

/* Agent-loop tests against a scripted mock HTTP backend; filesystem is the
 * real POSIX one pointed at a temp dir, so history/skills run for real. */

static int fails;

#define CHECK(cond, msg)                                                       \
    do {                                                                       \
        if (!(cond)) {                                                         \
            printf("FAIL: %s\n", msg);                                         \
            fails++;                                                           \
        }                                                                      \
    } while (0)

/* ---- mock http ---- */

#define MAX_CAP 16
static char *captured_req[MAX_CAP];
static int n_captured;
static const char *scripted[MAX_CAP];
static int n_scripted, script_pos;
static int fail_count;   /* transport failures before a scripted response */
static int fail_code = -1; /* what http_post returns while failing */
static const char *stream_script[MAX_CAP];
static int n_stream, stream_pos;

/* fragment collector */
static char frag_log[4096];
static int frag_calls;

static void collect_frag(void *ud, const char *frag)
{
    (void)ud;
    strcat(frag_log, frag);
    frag_calls++;
}

static void mock_reset(void)
{
    for (int i = 0; i < n_captured; i++)
        free(captured_req[i]);
    n_captured = 0;
    n_scripted = 0;
    script_pos = 0;
    fail_count = 0;
    fail_code = -1;
    n_stream = 0;
    stream_pos = 0;
    frag_log[0] = 0;
    frag_calls = 0;
}


static void mock_script(const char *resp)
{
    assert(n_scripted < MAX_CAP);
    scripted[n_scripted++] = resp;
}

static int mock_http_post(const char *url, const char *auth, const char *body,
                          size_t body_len, char *resp, size_t cap, size_t *rl)
{
    (void)url;
    (void)auth;
    (void)body_len;
    assert(n_captured < MAX_CAP);
    captured_req[n_captured++] = strdup(body);
    if (fail_count > 0) {
        fail_count--;
        return fail_code;
    }
    if (script_pos >= n_scripted)
        return -1;
    const char *r = scripted[script_pos++];
    size_t n = strlen(r);
    if (n > cap)
        n = cap;
    memcpy(resp, r, n);
    *rl = n;
    return 200;
}

/* ---- mock SSE stream: each scripted entry is a \n-separated list of
 * data-payload lines (no "data:" prefix) ---- */

static void mock_stream_script(const char *multi)
{
    assert(n_stream < MAX_CAP);
    stream_script[n_stream++] = multi;
}

static int mock_http_post_stream(const char *url, const char *auth,
                                 const char *body, size_t body_len,
                                 hl_line_fn on_line, void *ud,
                                 char *errbuf, size_t errcap)
{
    (void)url;
    (void)auth;
    (void)body_len;
    (void)errbuf;
    (void)errcap;
    assert(n_captured < MAX_CAP);
    captured_req[n_captured++] = strdup(body);
    if (stream_pos >= n_stream)
        return -1;
    char *tmp = strdup(stream_script[stream_pos++]);
    for (char *ln = strtok(tmp, "\n"); ln; ln = strtok(NULL, "\n"))
        if (on_line(ud, ln)) {
            free(tmp);
            return -3;
        }
    free(tmp);
    return 200;
}

static const char *resp_reply(const char *text)
{
    static char buf[1024];
    char *q = hl_json_quote_dyn(text);
    snprintf(buf, sizeof buf,
             "{\"choices\":[{\"message\":{\"role\":\"assistant\",\"content\":%s}}]}", q);
    free(q);
    return buf;
}

static const char *resp_tool_call(const char *id, const char *name, const char *args)
{
    static char buf[1024];
    char *q = hl_json_quote_dyn(args);
    snprintf(buf, sizeof buf,
             "{\"choices\":[{\"message\":{\"role\":\"assistant\",\"content\":null,"
             "\"tool_calls\":[{\"id\":\"%s\",\"type\":\"function\",\"function\":"
             "{\"name\":\"%s\",\"arguments\":%s}}]}}]}", id, name, q);
    free(q);
    return buf;
}

static const char *resp_two_calls(const char *id1, const char *args1,
                                  const char *id2, const char *args2)
{
    static char buf[1024];
    char *q1 = hl_json_quote_dyn(args1), *q2 = hl_json_quote_dyn(args2);
    snprintf(buf, sizeof buf,
             "{\"choices\":[{\"message\":{\"role\":\"assistant\",\"content\":null,"
             "\"tool_calls\":["
             "{\"id\":\"%s\",\"type\":\"function\",\"function\":{\"name\":\"add\",\"arguments\":%s}},"
             "{\"id\":\"%s\",\"type\":\"function\",\"function\":{\"name\":\"add\",\"arguments\":%s}}"
             "]}}]}", id1, q1, id2, q2);
    free(q1);
    free(q2);
    return buf;
}

/* ---- tools under test ---- */

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

static int tool_boom(void *ud, const char *args, char *out, size_t cap)
{
    (void)ud;
    (void)args;
    snprintf(out, cap, "error: exploded on purpose");
    return 1;
}

static hl_platform_t test_platform(void)
{
    hl_platform_t p = *hl_platform_posix();
    p.http_post = mock_http_post;
    p.http_post_stream = mock_http_post_stream;
    p.log = NULL;
    return p;
}

static hl_platform_t test_platform_nostream(void)
{
    hl_platform_t p = *hl_platform_posix();
    p.http_post = mock_http_post;
    p.http_post_stream = NULL;
    p.log = NULL;
    return p;
}

static char s_tmpdir[256];

static void make_tmpdir(void)
{
    snprintf(s_tmpdir, sizeof s_tmpdir, "/tmp/hl_test_%dXXXXXX", (int)getpid());
    assert(mkdtemp(s_tmpdir));
}

static hl_config_t base_cfg(void)
{
    static char hist_path[512];
    snprintf(hist_path, sizeof hist_path, "%s/history.jsonl", s_tmpdir);
    hl_config_t cfg = { 0 };
    memset(&cfg, 0, sizeof cfg);
    cfg.api_url = "https://mock/v1/chat/completions";
    cfg.api_key = "test-key";
    cfg.model = "test-model";
    cfg.system_prompt = "be brief";
    cfg.history_path = hist_path;
    cfg.max_ctx_chars = 16000;
    return cfg;
}

static void test_simple_reply(void)
{
    mock_reset();
    mock_script(resp_reply("hello there"));
    hl_platform_t p = test_platform();
    hl_config_t cfg = base_cfg();
    hl_agent_t *a = hl_agent_new(&cfg, &p, NULL, 0);
    char reply[512];
    int r = hl_agent_chat(a, "hi", reply, sizeof reply);
    CHECK(r == 0, "simple: chat ok");
    CHECK(!strcmp(reply, "hello there"), "simple: reply text");
    CHECK(n_captured == 1, "simple: one request");
    CHECK(strstr(captured_req[0], "\"content\":\"hi\""), "simple: user msg sent");
    CHECK(strstr(captured_req[0], "be brief"), "simple: system prompt sent");
    CHECK(!strstr(captured_req[0], "\"tools\""), "simple: no tools section");
    hl_agent_free(a);
}

static void test_tool_loop(void)
{
    mock_reset();
    mock_script(resp_tool_call("call_1", "add", "{\"a\":1,\"b\":2}"));
    mock_script(resp_reply("the sum is 3"));
    hl_platform_t p = test_platform();
    hl_config_t cfg = base_cfg();
    hl_tool_t tools[] = {
        { .name = "add", .description = "add two ints",
          .parameters_json = "{\"type\":\"object\"}", .fn = tool_add },
    };
    hl_agent_t *a = hl_agent_new(&cfg, &p, tools, 1);
    char reply[512];
    int r = hl_agent_chat(a, "what is 1+2", reply, sizeof reply);
    CHECK(r == 0, "tool: chat ok");
    CHECK(!strcmp(reply, "the sum is 3"), "tool: final reply");
    CHECK(n_captured == 2, "tool: two requests");
    CHECK(strstr(captured_req[1], "\"tool_call_id\":\"call_1\""), "tool: result id");
    CHECK(strstr(captured_req[1], "\"content\":\"3\""), "tool: result value");
    CHECK(strstr(captured_req[1], "\"tool_call_id\":\"call_1\""), "tool: id replayed");
    /* the assistant tool_calls block must be replayed before the tool result */
    const char *asst = strstr(captured_req[1], "\"tool_calls\":[{");
    const char *toolm = strstr(captured_req[1], "\"role\":\"tool\"");
    CHECK(asst && toolm && asst < toolm, "tool: assistant block precedes result");
    hl_agent_free(a);
}

static void test_parallel_calls(void)
{
    mock_reset();
    mock_script(resp_two_calls("c1", "{\"a\":1,\"b\":2}", "c2", "{\"a\":10,\"b\":20}"));
    mock_script(resp_reply("both done"));
    hl_platform_t p = test_platform();
    hl_config_t cfg = base_cfg();
    hl_tool_t tools[] = {
        { .name = "add", .description = "", .parameters_json = "{}", .fn = tool_add },
    };
    hl_agent_t *a = hl_agent_new(&cfg, &p, tools, 1);
    char reply[512];
    int r = hl_agent_chat(a, "add twice", reply, sizeof reply);
    CHECK(r == 0, "parallel: ok");
    CHECK(strstr(captured_req[1], "\"content\":\"3\""), "parallel: first result");
    CHECK(strstr(captured_req[1], "\"content\":\"30\""), "parallel: second result");
    CHECK(strstr(captured_req[1], "\"tool_call_id\":\"c2\""), "parallel: second id");
    hl_agent_free(a);
}

static void test_unknown_tool(void)
{
    mock_reset();
    mock_script(resp_tool_call("call_x", "nope", "{}"));
    mock_script(resp_reply("ok"));
    hl_platform_t p = test_platform();
    hl_config_t cfg = base_cfg();
    hl_agent_t *a = hl_agent_new(&cfg, &p, NULL, 0);
    char reply[512];
    int r = hl_agent_chat(a, "use nope", reply, sizeof reply);
    CHECK(r == 0, "unknown: loop continues");
    CHECK(strstr(captured_req[1], "unknown tool 'nope'"), "unknown: error fed back");
    hl_agent_free(a);
}

static void test_tool_failure(void)
{
    mock_reset();
    mock_script(resp_tool_call("call_f", "boom", "{}"));
    mock_script(resp_reply("recovered"));
    hl_platform_t p = test_platform();
    hl_config_t cfg = base_cfg();
    hl_tool_t tools[] = {
        { .name = "boom", .description = "", .parameters_json = "{}", .fn = tool_boom },
    };
    hl_agent_t *a = hl_agent_new(&cfg, &p, tools, 1);
    char reply[512];
    int r = hl_agent_chat(a, "explode", reply, sizeof reply);
    CHECK(r == 0, "failure: loop continues");
    CHECK(strstr(captured_req[1], "exploded on purpose"), "failure: msg fed back");
    hl_agent_free(a);
}

static void test_iteration_limit(void)
{
    mock_reset();
    for (int i = 0; i < 12; i++)
        mock_script(resp_tool_call("call_i", "add", "{}"));
    hl_platform_t p = test_platform();
    hl_config_t cfg = base_cfg();
    hl_tool_t tools[] = {
        { .name = "add", .description = "", .parameters_json = "{}", .fn = tool_add },
    };
    cfg.max_tool_iters = 3;
    hl_agent_t *a = hl_agent_new(&cfg, &p, tools, 1);
    char reply[512];
    int r = hl_agent_chat(a, "loop forever", reply, sizeof reply);
    CHECK(r < 0, "limit: returns error");
    CHECK(strstr(reply, "iteration limit"), "limit: message");
    CHECK(n_captured == 3, "limit: exactly 3 iterations");
    hl_agent_free(a);
}

static void test_skills(void)
{
    mock_reset();
    mock_script(resp_tool_call("call_s", "read_skill", "{\"name\":\"demo\"}"));
    mock_script(resp_reply("did the skill"));
    hl_platform_t p = test_platform();

    char sdir[512], fpath[600], body[] =
        "---\nname: demo\ndescription: demo skill for tests\n---\nDo the demo thing.";
    snprintf(sdir, sizeof sdir, "%s/skills", s_tmpdir);
    snprintf(fpath, sizeof fpath, "%s/demo", sdir);
    assert(mkdir(sdir, 0755) == 0 || errno == EEXIST);
    assert(mkdir(fpath, 0755) == 0 || errno == EEXIST);
    snprintf(fpath, sizeof fpath, "%s/demo/SKILL.md", sdir);
    FILE *f = fopen(fpath, "w");
    assert(f);
    fputs(body, f);
    fclose(f);

    hl_config_t cfg = base_cfg();
    static char sdir_s[512];
    snprintf(sdir_s, sizeof sdir_s, "%s", sdir);
    cfg.skills_dir = sdir_s;

    hl_agent_t *a = hl_agent_new(&cfg, &p, NULL, 0);
    char reply[512];
    int r = hl_agent_chat(a, "use demo", reply, sizeof reply);
    CHECK(r == 0, "skills: ok");
    CHECK(strstr(captured_req[0], "demo: demo skill for tests"), "skills: index injected");
    CHECK(strstr(captured_req[1], "Do the demo thing."), "skills: body returned");
    hl_agent_free(a);
}

static void test_context_trim(void)
{
    mock_reset();
    char big[9000];
    memset(big, 'A', sizeof big - 1);
    big[sizeof big - 1] = 0;
    mock_script(resp_reply("first ok"));
    mock_script(resp_reply("second ok"));
    hl_platform_t p = test_platform();
    hl_config_t cfg = base_cfg();
    cfg.max_ctx_chars = 500;
    hl_agent_t *a = hl_agent_new(&cfg, &p, NULL, 0);
    char reply[64];
    char msg1[9100], msg2[9100];
    snprintf(msg1, sizeof msg1, "FIRSTMARKER %s", big);
    snprintf(msg2, sizeof msg2, "SECONDMARKER %s", big);
    hl_agent_chat(a, msg1, reply, sizeof reply);
    hl_agent_chat(a, msg2, reply, sizeof reply);
    CHECK(n_captured == 2, "trim: two turns");
    CHECK(strstr(captured_req[1], "SECONDMARKER"), "trim: latest kept");
    CHECK(!strstr(captured_req[1], "FIRSTMARKER"), "trim: oldest dropped");
    hl_agent_free(a);
}

static void test_history_reload(void)
{
    mock_reset();
    hl_platform_t p = test_platform();
    hl_config_t cfg = base_cfg();
    {
        mock_script(resp_reply("ack"));
        hl_agent_t *a = hl_agent_new(&cfg, &p, NULL, 0);
        char reply[64];
        hl_agent_chat(a, "PERSISTED user line", reply, sizeof reply);
        hl_agent_free(a);
    }
    mock_reset();
    mock_script(resp_reply("ack2"));
    hl_agent_t *b = hl_agent_new(&cfg, &p, NULL, 0);
    char reply[64];
    hl_agent_chat(b, "new question", reply, sizeof reply);
    CHECK(strstr(captured_req[0], "PERSISTED user line"), "reload: prior turn present");
    CHECK(strstr(captured_req[0], "new question"), "reload: new turn present");
    hl_agent_free(b);
}

static void test_stream_reply(void)
{
    mock_reset();
    mock_stream_script(
        "{\"choices\":[{\"delta\":{\"role\":\"assistant\",\"content\":\"Hel\"}}]}\n"
        "{\"choices\":[{\"delta\":{\"content\":\"lo w\"}}]}\n"
        "\n"
        "{\"choices\":[{\"delta\":{\"content\":\"orld!\"}}]}\n"
        "[DONE]");
    hl_platform_t p = test_platform();
    hl_config_t cfg = base_cfg();
    hl_agent_t *a = hl_agent_new(&cfg, &p, NULL, 0);
    char reply[512];
    int r = hl_agent_chat_stream(a, "hi", collect_frag, NULL, reply, sizeof reply);
    CHECK(r == 0, "stream: chat ok");
    CHECK(!strcmp(reply, "Hello world!"), "stream: assembled reply");
    CHECK(!strcmp(frag_log, "Hello world!"), "stream: fragments live");
    CHECK(frag_calls == 3, "stream: three fragments");
    CHECK(strstr(captured_req[0], "\"stream\":true"), "stream: request flagged");
    hl_agent_free(a);
}

static void test_stream_tool_call(void)
{
    mock_reset();
    /* turn 1: arguments split across two deltas */
    mock_stream_script(
        "{\"choices\":[{\"delta\":{\"tool_calls\":[{\"index\":0,\"id\":\"c1\","
        "\"function\":{\"name\":\"add\",\"arguments\":\"{\\\"a\\\"\"}}]}}]}\n"
        "{\"choices\":[{\"delta\":{\"tool_calls\":[{\"index\":0,"
        "\"function\":{\"arguments\":\":1,\\\"b\\\":2}\"}}]}}]}\n"
        "[DONE]");
    /* turn 2: plain streamed reply */
    mock_stream_script(
        "{\"choices\":[{\"delta\":{\"content\":\"3 it is\"}}]}\n"
        "[DONE]");
    hl_platform_t p = test_platform();
    hl_config_t cfg = base_cfg();
    hl_tool_t tools[] = {
        { .name = "add", .description = "", .parameters_json = "{}", .fn = tool_add },
    };
    hl_agent_t *a = hl_agent_new(&cfg, &p, tools, 1);
    char reply[512];
    int r = hl_agent_chat_stream(a, "add 1 2", collect_frag, NULL, reply, sizeof reply);
    CHECK(r == 0, "stream tool: ok");
    CHECK(!strcmp(reply, "3 it is"), "stream tool: final reply");
    CHECK(n_captured == 2, "stream tool: two turns");
    CHECK(strstr(captured_req[1], "\"tool_call_id\":\"c1\""), "stream tool: id replayed");
    CHECK(strstr(captured_req[1], "\"content\":\"3\""), "stream tool: result value");
    /* assistant block must carry the reassembled arguments */
    CHECK(strstr(captured_req[1], "\\\"a\\\":1,\\\"b\\\":2"), "stream tool: args replayed");
    hl_agent_free(a);
}

static void test_stream_fallback(void)
{
    mock_reset();
    mock_script(resp_reply("fallback text"));
    hl_platform_t p = test_platform_nostream();
    hl_config_t cfg = base_cfg();
    hl_agent_t *a = hl_agent_new(&cfg, &p, NULL, 0);
    char reply[512];
    int r = hl_agent_chat_stream(a, "hi", collect_frag, NULL, reply, sizeof reply);
    CHECK(r == 0, "fallback: ok");
    CHECK(!strcmp(reply, "fallback text"), "fallback: reply");
    CHECK(frag_calls == 1 && !strcmp(frag_log, "fallback text"),
          "fallback: on_text fired once with whole text");
    CHECK(strstr(captured_req[0], "\"stream\":false"), "fallback: buffered request");
    hl_agent_free(a);
}

static void test_http_retry(void)
{
    mock_reset();
    fail_count = 2; /* two transport failures, then the scripted reply */
    mock_script(resp_reply("recovered after retries"));
    hl_platform_t p = test_platform();
    hl_config_t cfg = base_cfg();
    hl_agent_t *a = hl_agent_new(&cfg, &p, NULL, 0);
    char reply[512];
    int r = hl_agent_chat(a, "hello", reply, sizeof reply);
    CHECK(r == 0, "retry: chat ok");
    CHECK(!strcmp(reply, "recovered after retries"), "retry: reply text");
    CHECK(n_captured == 3, "retry: three attempts made");
    hl_agent_free(a);
}

static void test_no_retry_on_overflow(void)
{
    mock_reset();
    fail_count = 1;
    fail_code = -2; /* overflow is terminal: never retried */
    mock_script(resp_reply("never reached"));
    hl_platform_t p = test_platform();
    hl_config_t cfg = base_cfg();
    hl_agent_t *a = hl_agent_new(&cfg, &p, NULL, 0);
    char reply[512];
    int r = hl_agent_chat(a, "hello", reply, sizeof reply);
    CHECK(r < 0, "overflow: chat fails");
    CHECK(n_captured == 1, "overflow: not retried");
    hl_agent_free(a);
}

static void test_history_compact(void)
{
    mock_reset();
    hl_platform_t p = test_platform();
    hl_config_t cfg = base_cfg();
    static char hpath[512];
    snprintf(hpath, sizeof hpath, "%s/compact.jsonl", s_tmpdir);
    cfg.history_path = hpath;
    cfg.buf_hist = 2048;      /* compact triggers past 1536 bytes */
    cfg.max_ctx_chars = 900;  /* keep_chars floor: 6144 → last ~6 msgs */

    hl_agent_t *a = hl_agent_new(&cfg, &p, NULL, 0);
    char big[400];
    memset(big, 'x', sizeof big - 1);
    big[sizeof big - 1] = 0;
    char reply[64];
    for (int i = 0; i < 6; i++) {
        char msg[512];
        snprintf(msg, sizeof msg, "MSG%d %s", i, big);
        mock_script(resp_reply("ok"));
        hl_agent_chat(a, msg, reply, sizeof reply);
    }
    hl_agent_free(a);

    /* file must have been rewritten (not just appended) and stay bounded */
    FILE *f = fopen(hpath, "rb");
    CHECK(f != NULL, "compact: file exists");
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fclose(f);
    CHECK(size < 8192, "compact: file bounded (<8KB)");

    /* latest turns must survive compaction */
    char buf[16384];
    size_t len = 0;
    hl_platform_posix()->fs_read(hpath, buf, sizeof buf - 1, &len);
    buf[len] = 0;
    CHECK(strstr(buf, "MSG5"), "compact: latest user msg kept");
    CHECK(strstr(buf, "MSG0") == NULL, "compact: oldest msg dropped");
}

int main(void)
{
    make_tmpdir();
    test_simple_reply();
    test_tool_loop();
    test_parallel_calls();
    test_unknown_tool();
    test_tool_failure();
    test_iteration_limit();
    test_skills();
    test_context_trim();
    test_history_reload();
    test_stream_reply();
    test_stream_tool_call();
    test_stream_fallback();
    test_history_compact();
    test_http_retry();
    test_no_retry_on_overflow();
    if (fails) {
        printf("%d agent test(s) FAILED\n", fails);
        return 1;
    }
    printf("all agent tests passed\n");
    return 0;
}
