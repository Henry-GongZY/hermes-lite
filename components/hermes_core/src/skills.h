#ifndef HL_SKILLS_H
#define HL_SKILLS_H

#include "hermes_lite.h"
#include "json.h"

/* Build the "- name: description" index into out (one line per skill).
 * scratch/scap is file-reading scratch. Returns the number of skills. */
int hl_skills_build_index(const hl_platform_t *p, const char *dir, sbuf_t *out,
                          char *scratch, size_t scap);

/* Load the full markdown body of a skill into buf (caller sizes cap+1).
 * Returns the body length, or -1 if the skill does not exist. */
int hl_skills_load(const hl_platform_t *p, const char *dir, const char *name,
                   char *buf, size_t cap);

#endif
