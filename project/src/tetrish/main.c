// Main REPL for tetrish, the launcher/control shell for the tetriSH system.
#include <stdio.h> // standard input/output functions
#include <stdlib.h> // standard library functions
#include <string.h> // string manipualtion functions
#include <unistd.h> // provides access to POSIX OS API functions
#include <sys/wait.h> // provides functions for process management and waiting
#include <errno.h> // error reporting mechanism
#include "rc.h"
#include <signal.h>

#define MAX_ARGS 64 // argv slots; enough for ashell line
char input[512];

extern char **environ; // the process environment array, used by the `env` builtin

// Use volatile sig_atomic_t for signal flags
volatile sig_atomic_t interrupted = 0;

// Signal handler updates the flag
void handle_signal(int sig){
  (void)sig;            // parameter unused; silence -Wextra
  interrupted = 1;
}

// --- BUILTINS ---
// Each builtin runs INSIDE the shell process (so it can change our directory,
// environment, or end the loop which a forked child could not).
// Convention: return 0 to keep looping, non-zero to exit the shell.

// cd: change the shell's working directory. No arg -> go to $HOME.
static int cmd_cd(char **argv){
  const char *dir = argv[1] ? argv[1] : getenv("HOME");
  if (dir == NULL){
    fprintf(stderr, "cd: no directory given and HOME is not set\n");
    return 0;
  }
  if (chdir(dir) != 0)  // chdir() changes THIS process's working directory
    perror("cd"); // If chdir failed (e.g no such dir), print why
  return 0;
}

// exit: the only builtin that ends the REPL.
static int cmd_exit(char **argv){
  (void)argv;
  return 1;
}

// env: print every variable in the environment.
static int cmd_env(char **argv){
  (void)argv;
  for (char **e = environ; *e != NULL; e++)
    printf("%s\n", *e);
  return 0;
}

// setenv NAME VALUE: set/overwrite an environment variable IN THIS SHELL, so
// every program we later fork inherits it.
static int cmd_setenv(char **argv){
  if (argv[1] == NULL || argv[2] == NULL){
    fprintf(stderr, "usage: setenv NAME VALUE\n");
    return 0;
  }
  if (setenv(argv[1], argv[2], 1) != 0)   // the 1 means "overwrite if it exists"
    perror("setenv");
  return 0;
}

// unsetenv NAME: remove a variable from the environment.
static int cmd_unsetenv(char **argv){
  if (argv[1] == NULL){
    fprintf(stderr, "usage: unsetenv NAME\n");
    return 0;
  }
  if (unsetenv(argv[1]) != 0)
    perror("unsetenv");
  return 0;
}

// help: list what the shell understands.
static int cmd_help(char **argv){
  (void)argv;
  printf("tetrish builtins : cd help exit usage env setenv unsetenv\n");
  printf("stubs (later)    : sys dspawn dcheck\n");
  printf("anything else is run as an external program (via fork/execvp).\n");
  return 0;
}

// usage: how to invoke the shell itself.
static int cmd_usage(char **argv){
  (void)argv;
  printf("usage: tetrish [config-file]   (default config: .tetrishrc)\n");
  printf("type 'help' to list builtins.\n");
  return 0;
}

// stub: shared handler for commands not built yet (sys, dspawn, dcheck).
static int cmd_stub(char **argv){
  printf("%s: not yet implemented\n", argv[0]);
  return 0;
}

// --- dispatch table: name -> function, matched by index ---
// A function pointer type: "a function taking char** and returning int".
typedef int (*builtin_fn)(char **argv);

// These are the words the user types
static const char *names[] = {
  "cd", "help", "exit", "usage", "env", "setenv", "unsetenv",
  "sys", "dspawn", "dcheck"
};

// These are the functions to run when the user types the above words.
static const builtin_fn funcs[] = {
  cmd_cd, cmd_help, cmd_exit, cmd_usage, cmd_env, cmd_setenv, cmd_unsetenv,
  cmd_stub, cmd_stub, cmd_stub          // the last three share one stub
};
#define N_BUILTINS ((int)(sizeof(names) / sizeof(names[0])))

// --- EXTERNAL COMMANDS ---
// Not a builtin? Run it as a real program: fork a child, replace the child's
// image with execvp, and have the parent (the shell) wait for it to finish.
static int launch(char **argv){
  pid_t pid = fork();
  if (pid < 0){
    perror("fork");                 // fork failed -> report, keep the shell alive
    return 0;
  }
  if (pid == 0){
    // CHILD: become the requested program. execvp searches $PATH and needs the
    // NULL-terminated argv our tokenizer built.
    execvp(argv[0], argv);
    // execvp ONLY returns if it failed (bad command, no permission, ...).
    fprintf(stderr, "tetrish: %s: %s\n", argv[0], strerror(errno));
    _exit(127);                     // _exit (not exit) in a child: no stdio double-flush
  }
  // PARENT: reap the child so it does not linger as a zombie.
  int status;
  waitpid(pid, &status, 0);
  return 0;
}

// --- TOKENIZER ---
// Split `line` in place into argv[] on spaces/tabs. Returns argc.
// argv ends with a NULL entry, exactly the shape execvp() expects.
static int split_line(char *line, char **out, int max){
  int n = 0;
  char *saveptr;                              // strtok_r's state (reentrant)
  char *tok = strtok_r(line, " \t", &saveptr);
  while (tok != NULL && n < max - 1){         // leave one slot for the NULL
    out[n++] = tok;
    tok = strtok_r(NULL, " \t", &saveptr);
  }
  out[n] = NULL;
  return n;
}

int main(int argc, char **argv){
  // Set up a signal handler for ctrl + c or input quit
  signal(SIGINT, handle_signal);

  // Creating a config file parser
  // Provides functionality to read, write and modify config files
  // Check if there are more than one command line arguments
  // (load config BEFORE the banner: refuse to start silently on bad config)
  const char *rc_path = (argc > 1) ? argv[1] : ".tetrishrc";
  Config config;
  // Error case, if configuration loading fails
  if (rc_load(rc_path, &config) != 0){
    // print error and exit
    fprintf(stderr, "Failed to load configuration from %s\n", rc_path);
    return 1; // Exit with error code
  }

  printf("Tetrish REPL - Type 'exit' or Ctrl + C to exit\n");

  // Now the REPL shell needs to loop forever
  while (1){
    // Need the prompt to appear immediately without any delays
    printf("tetrish> ");
    fflush(stdout);

    // Clear the interrupted flag before reading
    interrupted = 0;

    // Read the input from user via stdin
    // Main idea: Ctrl + D to exit the program and Ctrl + C to return to prompt
    // Check if fgets fails to read from stdin
    if (fgets(input, sizeof(input), stdin) == NULL){
      if (feof(stdin)) {
        printf("\nEOF received, exiting....\n");
        break;
      } else if (interrupted){
        // User pressed Ctrl+C during input
        printf("\nInterrupted by user\n");
        continue;
      } else {
      // Some other error occured
      printf("\nError reading input\n");
      continue;
    }
    }
    // Remove the trailing newline character added by fgets
    input[strcspn(input, "\n")] = '\0';

    // Split the line into a command word + its arguments.
    char *cmdv[MAX_ARGS];
    int n = split_line(input, cmdv, MAX_ARGS);
    if (n == 0)                 // blank / whitespace-only line -> reprompt
      continue;

    // Is the command word one of our builtins? Walk the dispatch table.
    int handled = 0, want_exit = 0;
    for (int i = 0; i < N_BUILTINS; i++){
      if (strcmp(cmdv[0], names[i]) == 0){
        want_exit = funcs[i](cmdv);   // call the matching handler through its pointer
        handled = 1;
        break;
      }
    }
    if (want_exit)              // cmd_exit asked us to quit
      break;
    if (handled)               // a builtin ran -> next prompt
      continue;

    // Not a builtin -> treat it as an external program.
    launch(cmdv);
  }
  return 0;
}
