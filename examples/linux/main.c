#include "hermes_lite.h"
#include "json.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* Terminal demo: chat with tools (exec/time). The exec tool gives the agent
 * a shell — dev-machine only, obviously not for shipping. */

static int tool_time_now(void *ud, const char *args, char *out, size_t cap)
{
    (void)ud;
    (void)args;
    time_t t = time(NULL);
    struct tm tm;
    localtime_r(&t, &tm);
    strftime(out, cap, "%Y-%m-%d %H:%M:%S", &tm);
    return 0;
}

static int tool_exec(void *ud, const char *args, char *out, size_t cap)
{
    (void)ud;
    char arena[1024];
    jval_t *v = hl_json_parse(arena, sizeof arena, args && *args ? args : "{}", NULL);
    jval_t *cmd = hl_jobj_get(v, "command");
    if (!cmd || cmd->type != J_STR || !cmd->str[0]) {
        snprintf(out, cap, "error: missing 'command'");
        return 1;
    }
    FILE *f = popen(cmd->str, "r");
    if (!f) {
        snprintf(out, cap, "error: popen failed");
        return 1;
    }
    size_t n = fread(out, 1, cap - 1, f);
    int status = pclose(f);
    out[n] = 0;
    if (n == 0)
        snprintf(out, cap, "(no output, exit=%d)", status);
    return 0;
}

static void on_fragment(void *ud, const char *frag)
{
    (void)ud;
    fputs(frag, stdout);
    fflush(stdout);
}

int main(void)
{
    const char *url = getenv("HL_API_URL");
    const char *key = getenv("HL_API_KEY");
    const char *model = getenv("HL_MODEL");
    const char *skills = getenv("HL_SKILLS_DIR");
    const char *hist = getenv("HL_HISTORY");

    if (!key || !key[0]) {
        fprintf(stderr, "usage: HL_API_KEY=sk-... [%s]\n", "./hermes-lite");
        return 1;
    }

    hl_config_t cfg = {
        .api_url = url && url[0] ? url : "https://www.sui-xiang.net/v1/chat/completions",
        .api_key = key,
        .model = model && model[0] ? model : "gpt-6.1-terra",
        .system_prompt =
            "You are Hermes Lite, a concise assistant running on the user's machine. "
            "When a task needs a shell command or current time, use your tools "
            "instead of guessing.",
        .skills_dir = skills && skills[0] ? skills : "skills",
        .history_path = hist && hist[0] ? hist : "history.jsonl",
        .max_ctx_chars = 24000,
    };

    hl_tool_t tools[] = {
        { .name = "exec",
          .description = "Run a shell command and return its combined stdout. "
                         "Working directory is the process cwd.",
          .parameters_json =
              "{\"type\":\"object\",\"properties\":{\"command\":{\"type\":\"string\","
              "\"description\":\"the shell command to run\"}},\"required\":[\"command\"]}",
          .fn = tool_exec },
        { .name = "time_now",
          .description = "Returns the current local date and time.",
          .parameters_json = "{\"type\":\"object\",\"properties\":{}}",
          .fn = tool_time_now },
    };

    hl_agent_t *agent = hl_agent_new(&cfg, hl_platform_posix(), tools, 2);
    if (!agent) {
        fprintf(stderr, "agent init failed\n");
        return 1;
    }

    printf("hermes-lite | model=%s | type 'quit' to exit\n", cfg.model);
    char line[2048], reply[8192];
    for (;;) {
        printf("\x1b[36myou>\x1b[0m ");
        fflush(stdout);
        if (!fgets(line, sizeof line, stdin))
            break;
        line[strcspn(line, "\r\n")] = 0;
        if (!line[0])
            continue;
        if (!strcmp(line, "quit") || !strcmp(line, "exit"))
            break;
        long t0 = hl_platform_posix()->time_ms();
        printf("\x1b[33mhermes>\x1b[0m ");
        fflush(stdout);
        int r = hl_agent_chat_stream(agent, line, on_fragment, NULL, reply, sizeof reply);
        long dt = hl_platform_posix()->time_ms() - t0;
        if (r == 0)
            printf("\x1b[2m(%lu ms)\x1b[0m\n", dt);
        else
            printf("\n\x1b[31merror\x1b[0m: %s\n", reply[0] ? reply : "see log");
    }
    hl_agent_free(agent);
    return 0;
}
