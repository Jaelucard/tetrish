// Unit tests for control_get() (src/tetrish-view/control.c) against a mock
// control-plane socket (tests/mocks/mock_control.c). This proves the full
// path end to end: request bytes actually sent, response actually parsed.
//
// Needs libhtttp (control_get itself is built on it), so unlike
// test_view_repl this is a regular test binary, not part of the sanitized,
// no-teammate-library "test-san" tier.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "control.h"
#include "mocks/mock_control.h"

static int failures = 0;

static void check(const char *what, int cond){
    printf("  %-58s %s\n", what, cond ? "ok" : "FAIL");
    if (!cond) failures++;
}

int main(void){
    printf("test_control: control_get against a mock control-plane socket\n");

    {
        mock_control_t mock;
        Config cfg;
        const char *canned = "{\"uptime_seconds\": 42, \"rooms\": 3, \"players\": 5}";
        if (mock_control_start(&mock, &cfg, 200, canned) != 0){
            printf("  FAIL setup: could not start mock_control\n");
            return 1;
        }

        char body[4096];
        int st = control_get(&cfg, "/status", body, sizeof body);
        mock_control_stop(&mock);

        check("control_get returns the mocked status",  st == 200);
        check("control_get returns the mocked body",     strcmp(body, canned) == 0);
        check("the request line asked for GET /status",
              strstr(mock.got_request, "GET /status") != NULL);
        check("the request used the HTTTP wire version",
              strstr(mock.got_request, "HTTTP/") != NULL);
    }

    {
        // A non-200 status must still come through with its body: the repl's
        // cmd_status prints whatever body arrived and only treats a non-200
        // as a separate failure signal, so control_get must not swallow one
        // or the other.
        mock_control_t mock;
        Config cfg;
        if (mock_control_start(&mock, &cfg, 500, "server exploded") != 0){
            printf("  FAIL setup: could not start mock_control\n");
            return 1;
        }
        char body[256];
        int st = control_get(&cfg, "/status", body, sizeof body);
        mock_control_stop(&mock);
        check("a non-200 status is still reported",    st == 500);
        check("...with its body still delivered",      strcmp(body, "server exploded") == 0);
    }

    {
        // No listener at all is the ordinary case for someone just trying
        // the console without tetrisd running -- control_get must fail
        // cleanly (-1), never hang or crash.
        Config cfg;
        memset(&cfg, 0, sizeof cfg);
        snprintf(cfg.ctl_path, sizeof cfg.ctl_path, "/tmp/tv-mock-control-does-not-exist.sock");
        char body[64] = "unset";
        int st = control_get(&cfg, "/status", body, sizeof body);
        check("connecting to a socket that doesn't exist fails cleanly", st == -1);
        check("...and leaves the body empty rather than stale/garbage",
              body[0] == '\0');
    }

    // Week 10: repl.c's cmd_rooms/cmd_players/cmd_kick all go through this
    // same control_get(), just with different paths and canned bodies shaped
    // like jb_room/jb_player (src/tetrisd/main.c). jsonish.c's own field
    // parsing is covered by tests/test_view_json.c; this end checks that the
    // plumbing (request sent, status/body returned) generalizes beyond
    // /status rather than repeating jsonish's own assertions here.
    {
        mock_control_t mock;
        Config cfg;
        const char *canned = "[{\"id\": \"roomA\", \"players\": 2, \"started\": 1, \"ticks\": 480}]";
        if (mock_control_start(&mock, &cfg, 200, canned) != 0){
            printf("  FAIL setup: could not start mock_control\n");
            return 1;
        }
        char body[4096];
        int st = control_get(&cfg, "/rooms", body, sizeof body);
        mock_control_stop(&mock);

        check("control_get(/rooms) returns 200",           st == 200);
        check("control_get(/rooms) returns the mocked body", strcmp(body, canned) == 0);
        check("the request line asked for GET /rooms",
              strstr(mock.got_request, "GET /rooms") != NULL);
    }
    {
        mock_control_t mock;
        Config cfg;
        const char *canned = "[{\"player\": \"p7\", \"fd\": 5, \"room\": \"roomA\"}]";
        if (mock_control_start(&mock, &cfg, 200, canned) != 0){
            printf("  FAIL setup: could not start mock_control\n");
            return 1;
        }
        char body[4096];
        int st = control_get(&cfg, "/players", body, sizeof body);
        mock_control_stop(&mock);

        check("control_get(/players) returns 200",           st == 200);
        check("control_get(/players) returns the mocked body", strcmp(body, canned) == 0);
        check("the request line asked for GET /players",
              strstr(mock.got_request, "GET /players") != NULL);
    }
    {
        // No kick endpoint exists on the real tetrisd yet (see
        // REQUIRED_FILES.md); cmd_kick sends the request it would send
        // anyway and reports the server's real answer. Mirror that here:
        // this mock stands in for tetrisd's actual fallback response.
        mock_control_t mock;
        Config cfg;
        const char *canned = "{\"error\": \"unknown control path\"}";
        if (mock_control_start(&mock, &cfg, 404, canned) != 0){
            printf("  FAIL setup: could not start mock_control\n");
            return 1;
        }
        char body[4096];
        int st = control_get(&cfg, "/room/roomA/player/p7/kick", body, sizeof body);
        mock_control_stop(&mock);

        check("control_get(kick path) returns the mocked 404",  st == 404);
        check("...with tetrisd's real 'unknown control path' body",
              strcmp(body, canned) == 0);
        check("the request line asked for the exact kick path",
              strstr(mock.got_request, "GET /room/roomA/player/p7/kick") != NULL);
    }

    printf("\ntest_control: %s\n", failures ? "FAILED" : "ALL PASS");
    return failures ? 1 : 0;
}
