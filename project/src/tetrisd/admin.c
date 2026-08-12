#include <stdio.h>
#include <string.h>
#include "admin.h"

// Constant-time string compare: always walks the full length of `b` (the
// configured token) regardless of where `a` (the attacker-supplied token)
// first differs, so a timing side channel can't be used to guess a valid
// token one byte at a time. Not as important here as it would be for,
// say, comparing a session key, but cheap to get right and it is
// literally an authentication token, so we do it anyway.
static int consttime_streq(const char *a, const char *b) {
    size_t la = strlen(a), lb = strlen(b);
    if (la != lb) {
        // Still walk the comparison so *length* doesn't create as
        // simple a timing signal as it otherwise would.
        volatile int diff = 1;
        for (size_t i = 0; i < lb; i++) diff |= (i < la ? a[i] : 0) ^ b[i];
        return 0;
    }
    volatile int diff = 0;
    for (size_t i = 0; i < lb; i++) diff |= a[i] ^ b[i];
    return diff == 0;
}

static admin_role_t parse_role(const char *s) {
    if (strcmp(s, "readonly") == 0) return ADMIN_ROLE_READONLY;
    if (strcmp(s, "full") == 0) return ADMIN_ROLE_FULL;
    return ADMIN_ROLE_NONE;
}

int admin_table_load(const char *path, admin_table_t *out) {
    memset(out, 0, sizeof(*out));

    FILE *f = fopen(path, "r");
    if (!f) return 0;   // no file = no admin tokens configured, not an error

    char line[256];
    int lineno = 0;
    while (fgets(line, sizeof line, f)) {
        lineno++;
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '#' || *p == '\n' || *p == '\0') continue;

        char token[ADMIN_TOKEN_LEN], role_str[32];
        if (sscanf(p, "%64s %31s", token, role_str) != 2) {
            fprintf(stderr, "admin_table_load: %s:%d: malformed line, skipping\n", path, lineno);
            continue;
        }
        admin_role_t role = parse_role(role_str);
        if (role == ADMIN_ROLE_NONE) {
            fprintf(stderr, "admin_table_load: %s:%d: unknown role '%s', skipping\n", path, lineno, role_str);
            continue;
        }
        if (out->count >= ADMIN_MAX_TOKENS) {
            fprintf(stderr, "admin_table_load: %s: too many tokens (max %d), ignoring rest\n", path, ADMIN_MAX_TOKENS);
            break;
        }
        snprintf(out->entries[out->count].token, sizeof out->entries[0].token, "%s", token);
        out->entries[out->count].role = role;
        out->count++;
    }
    fclose(f);
    return 0;
}

admin_role_t admin_table_lookup(const admin_table_t *t, const char *token) {
    if (!token) return ADMIN_ROLE_NONE;
    admin_role_t found = ADMIN_ROLE_NONE;
    // Walk every entry regardless of an early match, so lookup time
    // doesn't leak *where* in the table a match happened either.
    for (int i = 0; i < t->count; i++) {
        if (consttime_streq(t->entries[i].token, token)) {
            found = t->entries[i].role;
        }
    }
    return found;
}

const char *admin_role_str(admin_role_t role) {
    switch (role) {
        case ADMIN_ROLE_READONLY: return "readonly";
        case ADMIN_ROLE_FULL: return "full";
        default: return "none";
    }
}

void admin_forbidden_body(char *buf, size_t bufsz, const char *method_str,
                          admin_role_t held, admin_role_t required) {
    snprintf(buf, bufsz,
             "{\"error\": \"%s requires the %s role\", \"your_role\": \"%s\", \"required_role\": \"%s\"}",
             method_str, admin_role_str(required), admin_role_str(held), admin_role_str(required));
}
