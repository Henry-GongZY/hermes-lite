#include "hermes_lite.h"
#include "json.h"

#include <string.h>
#include <stdio.h>

/* Skills are directories or flat markdown files following the agentskills.io
 * convention: a SKILL.md (or <name>.md) with YAML frontmatter carrying
 * name/description. Only the description is injected into the system prompt;
 * the LLM pulls the full body via the read_skill tool (progressive disclosure,
 * so a big skill library costs almost nothing per request). */

static int fm_field(const char *text, const char *field, char *out, size_t cap)
{
    size_t fl = strlen(field);
    for (const char *p = text; p && *p; ) {
        const char *eol = strchr(p, '\n');
        size_t ll = eol ? (size_t)(eol - p) : strlen(p);
        if (ll > fl + 1 && !strncmp(p, field, fl) && p[fl] == ':') {
            const char *v = p + fl + 1;
            while (*v == ' ')
                v++;
            size_t vl = (size_t)((p + ll) - v);
            while (vl && (v[vl - 1] == ' ' || v[vl - 1] == '\r'))
                vl--;
            if (vl >= cap)
                vl = cap - 1;
            memcpy(out, v, vl);
            out[vl] = 0;
            return 1;
        }
        p = eol ? eol + 1 : NULL;
    }
    return 0;
}

typedef struct {
    sbuf_t *out;
    const hl_platform_t *p;
    const char *dir;
    char *scratch;
    size_t scap;
    int count;
} skctx_t;

static int skill_cb(void *ud, const char *name)
{
    skctx_t *k = ud;
    if (!name[0] || name[0] == '.')
        return 0;
    char path[300];
    size_t len = 0;

    /* Directory listings differ by platform: POSIX yields the entry itself
     * ("my-skill" for a dir), SPIFFS-style VFS yields the full remainder
     * ("my-skill/SKILL.md"). Try the entry verbatim first, then the two
     * conventional layouts. A zero-byte read (directory on POSIX) counts
     * as a miss. */
    snprintf(path, sizeof path, "%s/%s", k->dir, name);
    if (k->p->fs_read(path, k->scratch, k->scap, &len) < 0 || len == 0) {
        snprintf(path, sizeof path, "%s/%s/SKILL.md", k->dir, name);
        if (k->p->fs_read(path, k->scratch, k->scap, &len) < 0 || len == 0) {
            snprintf(path, sizeof path, "%s/%s.md", k->dir, name);
            if (k->p->fs_read(path, k->scratch, k->scap, &len) < 0 || len == 0)
                return 0;
        }
    }
    k->scratch[len] = 0;

    char nm[64], ds[256];
    /* fallback display name: strip a trailing "/SKILL.md" or ".md" */
    snprintf(nm, sizeof nm, "%s", name);
    char *cut = strstr(nm, "/SKILL.md");
    if (cut)
        *cut = 0;
    else {
        cut = strrchr(nm, '.');
        if (cut && !strcmp(cut, ".md"))
            *cut = 0;
    }
    fm_field(k->scratch, "name", nm, sizeof nm); /* overrides fallback */
    if (!fm_field(k->scratch, "description", ds, sizeof ds))
        ds[0] = 0;

    hl_sb_puts(k->out, "\n- ");
    hl_sb_puts(k->out, nm);
    hl_sb_puts(k->out, ": ");
    hl_sb_puts(k->out, ds);
    k->count++;
    return 0;
}

int hl_skills_build_index(const hl_platform_t *p, const char *dir, sbuf_t *out,
                          char *scratch, size_t scap)
{
    skctx_t k = { out, p, dir, scratch, scap, 0 };
    p->fs_list(dir, skill_cb, &k);
    return k.count;
}

int hl_skills_load(const hl_platform_t *p, const char *dir, const char *name,
                   char *buf, size_t cap)
{
    if (!name[0] || name[0] == '.' || strchr(name, '/'))
        return -1; /* keep names flat: no traversal */
    char path[300];
    size_t len = 0;
    snprintf(path, sizeof path, "%s/%s/SKILL.md", dir, name);
    if (p->fs_read(path, buf, cap, &len) < 0) {
        snprintf(path, sizeof path, "%s/%s.md", dir, name);
        if (p->fs_read(path, buf, cap, &len) < 0)
            return -1;
    }
    buf[len] = 0;
    return (int)len;
}
