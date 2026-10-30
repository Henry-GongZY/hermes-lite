#include "json.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <assert.h>

static int fails;

#define CHECK(cond, msg)                                                       \
    do {                                                                       \
        if (!(cond)) {                                                         \
            printf("FAIL: %s\n", msg);                                         \
            fails++;                                                           \
        }                                                                      \
    } while (0)

static void test_parse_basic(void)
{
    static char arena[4096];
    const char *src =
        "{\"a\":1,\"b\":[true,false,null,\"x\\u4e2d\\\"y\"],\"c\":{\"d\":-2.5e2}}";
    jval_t *v = hl_json_parse(arena, sizeof arena, src, NULL);
    CHECK(v && v->type == J_OBJ, "parse object");
    jval_t *a = hl_jobj_get(v, "a");
    CHECK(a && a->type == J_NUM && a->num == 1.0, "int field");
    jval_t *b = hl_jobj_get(v, "b");
    CHECK(b && b->type == J_ARR && hl_jarr_len(b) == 4, "array len");
    CHECK(hl_jarr_get(b, 0)->boolean == 1, "true");
    CHECK(hl_jarr_get(b, 1)->boolean == 0, "false");
    CHECK(hl_jarr_get(b, 2)->type == J_NULL, "null");
    jval_t *s = hl_jarr_get(b, 3);
    CHECK(s && s->type == J_STR && !strcmp(s->str, "x\xe4\xb8\xad\"y"), "string escapes");
    jval_t *d = hl_jobj_get(hl_jobj_get(v, "c"), "d");
    CHECK(d && d->num == -250.0, "exponent number");
}

static void test_parse_errors(void)
{
    static char arena[512];
    CHECK(hl_json_parse(arena, sizeof arena, "{", NULL) == NULL, "truncated object");
    CHECK(hl_json_parse(arena, sizeof arena, "{\"a\":} ", NULL) == NULL, "missing value");
    CHECK(hl_json_parse(arena, sizeof arena, "[1,2]]", NULL) == NULL, "trailing garbage");
    CHECK(hl_json_parse(arena, sizeof arena, "\"bad\\q\"", NULL) == NULL, "bad escape");
    /* arena too small for the string payload */
    static char tiny[40];
    CHECK(hl_json_parse(tiny, sizeof tiny,
                        "[\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
                        "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\"]",
                        NULL) == NULL,
          "arena exhaustion");
}

static void test_arena_reuse(void)
{
    static char arena[4096];
    size_t used = 0;
    jval_t *v1 = hl_json_parse(arena, sizeof arena, "{\"x\":1}", &used);
    CHECK(v1 && used > 0, "first doc");
    jval_t *v2 = hl_json_parse(arena + used, sizeof arena - used, "{\"y\":2}", NULL);
    CHECK(v2 && hl_jobj_get(v2, "y")->num == 2.0, "second doc in same arena");
}

static void test_quote(void)
{
    char *q = hl_json_quote_dyn("a\"b\\c\nd\x01\xe4\xb8\xad");
    CHECK(q && !strcmp(q, "\"a\\\"b\\\\c\\nd\\u0001\xe4\xb8\xad\""), "quote escaping");
    free(q);
}

static void test_api_shape(void)
{
    static char arena[4096];
    const char *src =
        "{\"id\":\"resp_1\",\"choices\":[{\"index\":0,\"message\":{\"role\":"
        "\"assistant\",\"tool_calls\":[{\"id\":\"call_1\",\"type\":\"function\","
        "\"function\":{\"name\":\"time_now\",\"arguments\":\"{}\"}}]},"
        "\"finish_reason\":\"tool_calls\"}]}";
    jval_t *v = hl_json_parse(arena, sizeof arena, src, NULL);
    jval_t *msg = hl_jobj_get(hl_jarr_get(hl_jobj_get(v, "choices"), 0), "message");
    jval_t *tcs = hl_jobj_get(msg, "tool_calls");
    CHECK(tcs && hl_jarr_len(tcs) == 1, "tool_calls array");
    jval_t *fn = hl_jobj_get(hl_jarr_get(tcs, 0), "function");
    CHECK(fn && !strcmp(hl_jobj_get(fn, "name")->str, "time_now"), "tool name");
    CHECK(!strcmp(hl_jobj_get(fn, "arguments")->str, "{}"), "tool args");
}

int main(void)
{
    test_parse_basic();
    test_parse_errors();
    test_arena_reuse();
    test_quote();
    test_api_shape();
    if (fails) {
        printf("%d test(s) FAILED\n", fails);
        return 1;
    }
    printf("all json tests passed\n");
    return 0;
}
