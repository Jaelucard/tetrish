// Unit tests for the ops console repl's pure parsing/lookup logic
// (src/tetrish-view/dispatch.c): repl_split_line and repl_is_known_command.
// No socket, no tetrisd, no libhtttp -- deliberately, so this can build and
// run with no dependency on the session/protocol libraries (see the
// comment at the top of dispatch.c and the Makefile's
// SAN_EXTRA_SRC_test_view_repl).

#include <stdio.h>
#include <string.h>
#include "repl.h"

static int failures = 0;

static void check(const char *what, int cond){
    printf("  %-58s %s\n", what, cond ? "ok" : "FAIL");
    if (!cond) failures++;
}

int main(void){
    printf("test_view_repl: repl_split_line\n");
    {
        char line[] = "status";
        char *args = NULL;
        char *name = repl_split_line(line, &args);
        check("a bare command name splits to itself", strcmp(name, "status") == 0);
        check("no arguments leaves args empty", args != NULL && args[0] == '\0');
    }
    {
        char line[] = "kick p7";
        char *args = NULL;
        char *name = repl_split_line(line, &args);
        check("name/args split on the first space", strcmp(name, "kick") == 0);
        check("args is everything after the space", strcmp(args, "p7") == 0);
    }
    {
        char line[] = "  status   ";
        char *args = NULL;
        char *name = repl_split_line(line, &args);
        check("leading whitespace before the name is skipped", strcmp(name, "status") == 0);
        // Trailing whitespace with nothing after it: args is empty, not a
        // string of spaces -- repl_run strips the newline before calling
        // this, but trailing spaces on the line itself are legal input.
        check("trailing whitespace with no args yields an empty args string",
              args != NULL && args[0] == '\0');
    }
    {
        char line[] = "kick   p7   extra   words";
        char *args = NULL;
        char *name = repl_split_line(line, &args);
        check("only the FIRST run of whitespace splits name from args",
              strcmp(name, "kick") == 0);
        check("args keeps internal whitespace as the command sees fit to parse",
              strcmp(args, "p7   extra   words") == 0);
    }
    {
        char line[] = "";
        char *args = NULL;
        char *name = repl_split_line(line, &args);
        check("an empty line splits to an empty name", name[0] == '\0');
        check("...and an empty args", args != NULL && args[0] == '\0');
    }
    {
        char line[] = "   ";
        char *args = NULL;
        char *name = repl_split_line(line, &args);
        check("a whitespace-only line also splits to an empty name (repl_run's"
              " blank-line check)", name[0] == '\0');
    }

    printf("\ntest_view_repl: repl_is_known_command\n");
    check("status is a known command",       repl_is_known_command("status"));
    check("rooms is a known command",        repl_is_known_command("rooms"));
    check("players is a known command",      repl_is_known_command("players"));
    check("kick is a known command",         repl_is_known_command("kick"));
    check("attach is a known command",       repl_is_known_command("attach"));
    check("replay is a known command",       repl_is_known_command("replay"));
    check("STATUS (wrong case) is not known", !repl_is_known_command("STATUS"));
    check("an empty name is not known",       !repl_is_known_command(""));
    check("a nonsense name is not known",     !repl_is_known_command("bogus"));
    // help/quit/exit/? are handled directly by repl_run, not through the
    // command table, so they are correctly "not known" by this function's
    // own definition (see repl.h).
    check("help is handled by repl_run directly, not the command table",
          !repl_is_known_command("help"));
    check("quit is handled by repl_run directly, not the command table",
          !repl_is_known_command("quit"));

    printf("\ntest_view_repl: %s\n", failures ? "FAILED" : "ALL PASS");
    return failures ? 1 : 0;
}
