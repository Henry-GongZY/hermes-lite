#include "hermes_lite.h"
#include "json.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <errno.h>
#include <sys/stat.h>

/* Terminal demo: chat with tools (exec/time). The exec tool gives the agent
 * a shell — dev-machine only, obviously not for shipping. Sessions are
 * per-name JSONL history files under ./sessions/ (HL_SESSION picks the
 * initial session, default "default"). */

#define SESSIONS_DIR "sessions"
#define DEFAULT_SESSION "default"

/* The resolved path lives in a static buffer: hl_config_t.history_path is a
 * borrowed string that must outlive the agent, and the REPL's `line` buffer
 * is reused every command. */
static char s_session_path[512];

static const char *session_path(const char *name)
{
    snprintf(s_session_path, sizeof s_session_path, "%s/%s.jsonl", SESSIONS_DIR, name);
    return s_session_path;
}

/* Session names must be plain single path components (no separators,
 * no traversal, not hidden). */
static int valid_session_name(const char *name)
{
    if (!name || !name[0] || name[0] == '.')
        return 0;
    if (!strcmp(name, ".."))
        return 0;
    if (strchr(name, '/') || strchr(name, '\\'))
        return 0;
    return 1;
}

static int s_n_sessions;

static int sessions_file_cb(void *ud, const char *name)
{
    (void)ud;
    if (name[0] != '.') {
        printf("  %s\n", name);
        s_n_sessions++;
    }
    return 0;
}

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
    const char *session = getenv("HL_SESSION");
    if (!session || !session[0])
        session = DEFAULT_SESSION;

    if (!key || !key[0]) {
        fprintf(stderr, "usage: HL_API_KEY=sk-... [%s]\n", "./hermes-lite");
        return 1;
    }

    /* session storage: one JSONL history file per session name */
    if (mkdir(SESSIONS_DIR, 0755) != 0 && errno != EEXIST) {
        fprintf(stderr, "cannot create %s dir\n", SESSIONS_DIR);
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
        /* sessions/<name>.jsonl; the pointer is borrowed by the agent and
         * must stay valid for the agent's lifetime (static s_session_path) */
        .history_path = session_path(session),
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

    printf("hermes-lite | model=%s | session='%s' | commands: sessions | "
           "session <name> | quit\n", cfg.model, session);
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
        if (!strcmp(line, "sessions")) {
            printf("sessions in " SESSIONS_DIR "/:\n");
            s_n_sessions = 0;
            hl_platform_posix()->fs_list(SESSIONS_DIR, sessions_file_cb, NULL);
            if (!s_n_sessions)
                printf("  (none yet)\n");
            continue;
        }
        if (!strcmp(line, "session")) {
            printf("usage: session <name>\n");
            continue;
        }
        if (!strncmp(line, "session ", 8)) {
            const char *name = line + 8;
            while (*name == ' ')
                name++;
            if (!valid_session_name(name)) {
                printf("usage: session <name> (plain name, no '/', '..' or leading '.')\n");
            } else {
                hl_agent_free(agent);
                /* s_session_path (static) keeps the borrowed string alive */
                cfg.history_path = session_path(name);
                agent = hl_agent_new(&cfg, hl_platform_posix(), tools, 2);
                printf("session '%s' active (%s)\n", name, s_session_path);
            }
            continue;
        }
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
