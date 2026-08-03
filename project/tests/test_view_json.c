// Unit tests for the minimal control-plane JSON scanner
// (src/tetrish-view/jsonish.c), against the exact shapes tetrisd's /rooms
// and /players handlers produce (jb_room/jb_player in src/tetrisd/main.c).
// Pure text processing, no socket, no libhtttp.

#include <stdio.h>
#include <string.h>
#include "jsonish.h"

static int failures = 0;

static void check(const char *what, int cond){
    printf("  %-58s %s\n", what, cond ? "ok" : "FAIL");
    if (!cond) failures++;
}

int main(void){
    printf("test_view_json: jsonish_nth_object\n");
    {
        const char *rooms = "[{\"id\": \"roomA\", \"players\": 2, \"started\": 1, \"ticks\": 12345}, "
                             "{\"id\": \"roomB\", \"players\": 0, \"started\": 0, \"ticks\": 0}]";
        const char *o0 = jsonish_nth_object(rooms, 0);
        const char *o1 = jsonish_nth_object(rooms, 1);
        const char *o2 = jsonish_nth_object(rooms, 2);
        check("object 0 found",                  o0 != NULL && *o0 == '{');
        check("object 1 found",                  o1 != NULL && *o1 == '{');
        check("object 1 starts after object 0",  o1 > o0);
        check("object 2 does not exist",          o2 == NULL);
    }
    {
        const char *empty = "[]";
        check("an empty array has no object 0", jsonish_nth_object(empty, 0) == NULL);
    }
    {
        // A string field containing a brace must not confuse the counter.
        const char *tricky = "[{\"id\": \"room{weird}\", \"players\": 1}]";
        check("a brace inside a string value doesn't break object counting",
              jsonish_nth_object(tricky, 0) != NULL &&
              jsonish_nth_object(tricky, 1) == NULL);
    }

    printf("\ntest_view_json: jsonish_str / jsonish_int on a /rooms-shaped object\n");
    {
        const char *obj = "{\"id\": \"roomA\", \"players\": 2, \"started\": 1, \"ticks\": 12345}";
        char id[32]; long players = -99, started = -99, ticks = -99;
        check("id field found",       jsonish_str(obj, "id", id, sizeof id));
        check("id value correct",     strcmp(id, "roomA") == 0);
        check("players field found",  jsonish_int(obj, "players", &players));
        check("players value correct", players == 2);
        check("started field found",  jsonish_int(obj, "started", &started));
        check("started value correct", started == 1);
        check("ticks field found",    jsonish_int(obj, "ticks", &ticks));
        check("ticks value correct",  ticks == 12345);
        check("a field that isn't there is reported absent",
              !jsonish_int(obj, "nonexistent", &ticks));
    }

    printf("\ntest_view_json: jsonish_str / jsonish_int on a /players-shaped object\n");
    {
        const char *obj = "{\"player\": \"p7\", \"fd\": 5, \"room\": \"roomA\"}";
        char player[32], room[32]; long fd = -99;
        check("player field found",  jsonish_str(obj, "player", player, sizeof player));
        check("player value correct", strcmp(player, "p7") == 0);
        check("fd field found",       jsonish_int(obj, "fd", &fd));
        check("fd value correct",     fd == 5);
        check("room field found",     jsonish_str(obj, "room", room, sizeof room));
        check("room value correct",   strcmp(room, "roomA") == 0);
    }

    printf("\ntest_view_json: field lookups don't bleed across objects\n");
    {
        const char *arr = "[{\"id\": \"roomA\"}, {\"id\": \"roomB\"}]";
        const char *o0 = jsonish_nth_object(arr, 0);
        const char *o1 = jsonish_nth_object(arr, 1);
        char id0[32], id1[32];
        jsonish_str(o0, "id", id0, sizeof id0);
        jsonish_str(o1, "id", id1, sizeof id1);
        check("object 0's id is its own, not object 1's", strcmp(id0, "roomA") == 0);
        check("object 1's id is its own, not object 0's", strcmp(id1, "roomB") == 0);
    }

    printf("\ntest_view_json: a field name inside another field's VALUE\n");
    {
        // Room and player names come from clients (the JOIN path and the
        // handshake), and tetrisd's jb_room/jb_player do no escaping, so a
        // hostile name can embed the literal bytes `"room": "fake` inside
        // its own value. The scanner must still return the REAL room field,
        // not the decoy planted inside the player value.
        const char *obj =
            "{\"player\": \"p\"room\": \"fake\", \"fd\": 3, \"room\": \"real\"}";
        char room[32] = "";
        check("a decoy key inside a value does not shadow the real field",
              jsonish_str(obj, "room", room, sizeof room)
              && strcmp(room, "real") == 0);
    }

    printf("\ntest_view_json: truncation and negative numbers\n");
    {
        const char *obj = "{\"id\": \"a-very-long-room-name-that-does-not-fit\"}";
        char small[6];
        check("a value longer than the buffer is truncated, not overrun",
              jsonish_str(obj, "id", small, sizeof small) && strlen(small) < sizeof small);
    }
    {
        const char *obj = "{\"ticks\": -7}";
        long v = 0;
        check("a negative integer parses", jsonish_int(obj, "ticks", &v));
        check("...with the right value",   v == -7);
    }

    printf("\ntest_view_json: %s\n", failures ? "FAILED" : "ALL PASS");
    return failures ? 1 : 0;
}
