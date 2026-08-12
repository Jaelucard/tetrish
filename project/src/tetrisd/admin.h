#ifndef ADMIN_H
#define ADMIN_H

// admin.h — Week 9-11 deliverable: the ops-console permission model.
//
// Exactly two roles, as required for Week 11 ("read-only admin vs full
// admin"), strictly ordered (full includes everything readonly can do):
//
//   ADMIN_ROLE_READONLY   ADMIN-STATUS, ADMIN-ROOMS, ADMIN-ATTACH
//                         (monitoring + spectating a room's live STATE
//                          stream -- neither can change any game or
//                          server state)
//   ADMIN_ROLE_FULL        + ADMIN-KICK
//                         (the one mutating admin action exposed over
//                          the network protocol: remove a player from
//                          a room)
//
// Deliberately NOT in this table: anything that affects the daemon
// itself (shutting it down, forcing a room closed as opposed to kicking
// one player, editing the admin token table). Those stay on tetrisd's
// existing local-only Unix-domain-socket control plane (tetrisctl),
// which is a strictly stronger boundary than "holds the right network
// token" -- it additionally requires a local shell on the host. See
// the README's "Admin protocol" section for the full writeup of this
// decision, including why ADMIN-KICK is judged to be the right amount
// of "full admin" to expose here: it can only affect one player's
// connection, never the daemon's own availability.
//
// Tokens are matched by exact string equality (constant-time, to avoid a
// timing oracle on the token). One token maps to exactly one role; a
// deployment that wants several people at the same role just gives them
// each their own token line.
typedef enum {
    ADMIN_ROLE_NONE = 0,       // unrecognised token: full 401
    ADMIN_ROLE_READONLY = 1,
    ADMIN_ROLE_FULL = 2,
} admin_role_t;

#define ADMIN_MAX_TOKENS 16
#define ADMIN_TOKEN_LEN  65

typedef struct {
    char token[ADMIN_TOKEN_LEN];
    admin_role_t role;
} admin_entry_t;

typedef struct {
    admin_entry_t entries[ADMIN_MAX_TOKENS];
    int count;
} admin_table_t;

// Loads "<token> <role>\n" lines (role is "readonly" or "full", "#"
// starts a comment, blank lines ignored) from `path`. Returns 0 on
// success (including "file does not exist", which just means no admin
// tokens are configured -- ADMIN-* requests will all get 401), -1 on a
// malformed file.
int admin_table_load(const char *path, admin_table_t *out);

// Constant-time-ish lookup (see admin.c). Returns ADMIN_ROLE_NONE if the
// token isn't in the table.
admin_role_t admin_table_lookup(const admin_table_t *t, const char *token);

const char *admin_role_str(admin_role_t role);

// Week 11: "rejection returns 403 with a useful reason." Builds a JSON
// body naming both the method that was denied and the role gap (what the
// caller has vs. what they'd need), e.g.:
//   {"error": "ADMIN-KICK requires the full role", "your_role": "readonly", "required_role": "full"}
// `buf` must be at least 160 bytes.
void admin_forbidden_body(char *buf, size_t bufsz, const char *method_str,
                          admin_role_t held, admin_role_t required);

#endif
