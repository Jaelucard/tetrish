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

// --- BACKGROUND JOBS (dspawn / sys / dcheck) ---
// Every background program started with dspawn goes in a linked list, one
// item per job.
// Only the main loop reads or changes this list. The handler that runs when a
// background child finishes never touches it, it just sets a flag. The handler
// stays that small on purpose: a signal handler cannot safely call malloc or
// printf.
typedef enum { JOB_RUNNING, JOB_DONE } JobStatus;
typedef struct Job {
  pid_t pid;
  char cmd[64];          // the name of the program, so the `sys` listing is readable to a person
  JobStatus status;
  int exit_code;         // this value only means something once status is JOB_DONE
  struct Job *next;
} Job;
static Job *jobs = NULL;                        // the start of the list (we add new jobs to the front)

static volatile sig_atomic_t child_exited = 0;  // set to 1 when a child has finished, so we know to clean it up soon
static void handle_sigchld(int sig){
  (void)sig;
  child_exited = 1;      // setting this flag is the only thing we do here, to keep the handler safe
}

// Add a background child that we just started to the job list.
static void job_add(pid_t pid, const char *cmd){
  Job *j = malloc(sizeof *j);
  if (j == NULL){        // if malloc fails the child still runs, we just cannot track it in our list
    perror("dspawn: malloc");
    return;
  }
  j->pid = pid;
  snprintf(j->cmd, sizeof j->cmd, "%s", cmd);   // snprintf always adds a string terminator, so the name is safe
  j->status = JOB_RUNNING;
  j->exit_code = 0;
  j->next = jobs;        // link this new job in at the front of the list
  jobs = j;
}

// Clean up after every background child that has finished, so none of them
// hang around as zombies.
// WNOHANG means "return straight away instead of waiting", so this never holds
// up the prompt. A return of 0 means that child is still running.
// waitpid is called once per specific pid, never with -1. With -1 it could
// collect a foreground child that launch() is still waiting for, and launch()
// would lose track of it.
static void reap_jobs(void){
  child_exited = 0;
  for (Job *j = jobs; j != NULL; j = j->next){
    if (j->status != JOB_RUNNING)
      continue;
    int st;
    if (waitpid(j->pid, &st, WNOHANG) == j->pid){
      j->status = JOB_DONE;
      // If the program ended normally we store its exit code. If a signal
      // killed it we store 128 plus the signal number instead, which is the
      // same rule the bash shell uses.
      j->exit_code = WIFEXITED(st) ? WEXITSTATUS(st) : 128 + WTERMSIG(st);
    }
  }
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
  printf("job control      : dspawn CMD [ARGS...]   run CMD in the background\n");
  printf("                   sys                    list background jobs\n");
  printf("                   dcheck PID             poll one job (non-blocking)\n");
  printf("anything else is run as an external program (via fork/execvp).\n");
  return 0;
}

// sys: show every background job we are tracking and whether it is still running.
static int cmd_sys(char **argv){
  (void)argv;
  reap_jobs();                         // update each job's status before we print the list
  if (jobs == NULL){
    printf("sys: no background jobs\n");
    return 0;
  }
  for (Job *j = jobs; j != NULL; j = j->next){
    if (j->status == JOB_RUNNING)
      printf("[pid %d] RUNNING   %s\n", (int)j->pid, j->cmd);
    else
      printf("[pid %d] DONE(%d)   %s\n", (int)j->pid, j->exit_code, j->cmd);
  }
  return 0;
}

// dspawn CMD [ARGS...]: start CMD as a background job. Same fork and execvp as
// launch(), with two differences. The parent does not wait; it records the pid
// and goes back to the prompt. And the child is moved into its own process
// group, so a Ctrl+C aimed at the shell's foreground work cannot reach the
// background jobs.
static int cmd_dspawn(char **argv){
  if (argv[1] == NULL){
    fprintf(stderr, "usage: dspawn COMMAND [ARGS...]\n");
    return 0;
  }
  pid_t pid = fork();
  if (pid < 0){
    perror("dspawn: fork");
    return 0;
  }
  if (pid == 0){
    setpgid(0, 0);                     // move this child out of the terminal's foreground group
    execvp(argv[1], &argv[1]);         // replace the child with CMD; argv[1] onward is the command and its arguments
    fprintf(stderr, "dspawn: %s: %s\n", argv[1], strerror(errno));
    _exit(127);
  }
  job_add(pid, argv[1]);
  printf("[dspawn] started '%s' (pid %d)\n", argv[1], (int)pid);
  return 0;
}

// dcheck PID: check one job without waiting. Still running, we say so.
// Finished, we print its exit code and drop it from the list, because a
// finished job is only reported once.
static int cmd_dcheck(char **argv){
  if (argv[1] == NULL){
    fprintf(stderr, "usage: dcheck PID\n");
    return 0;
  }
  char *end;
  long v = strtol(argv[1], &end, 10);
  if (end == argv[1] || *end != '\0' || v <= 0){
    fprintf(stderr, "dcheck: '%s' is not a PID\n", argv[1]);
    return 0;
  }
  reap_jobs();                         // update statuses first, in case this child just finished
  Job *prev = NULL;
  for (Job *j = jobs; j != NULL; prev = j, j = j->next){
    if (j->pid != (pid_t)v)
      continue;
    if (j->status == JOB_RUNNING){
      printf("dcheck: pid %ld (%s) still running\n", v, j->cmd);
    } else {
      printf("dcheck: pid %ld (%s) exited with status %d\n", v, j->cmd, j->exit_code);
      // Unlink it. If something came before it, point that past it, otherwise
      // it was the head, so move the head forward.
      if (prev) prev->next = j->next;
      else      jobs = j->next;
      free(j);                         // free the memory now that we have reported this job
    }
    return 0;
  }
  printf("dcheck: pid %ld is not a tracked background job\n", v);
  return 0;
}

// usage: how to invoke the shell itself.
static int cmd_usage(char **argv){
  (void)argv;
  printf("usage: tetrish [config-file]   (default config: .tetrishrc)\n");
  printf("type 'help' to list builtins.\n");
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
  cmd_sys, cmd_dspawn, cmd_dcheck
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
  // The EINTR retry matters now that SIGINT has no SA_RESTART: a Ctrl+C aimed
  // at the foreground child interrupts OUR waitpid too. Drop the retry and we
  // abandon the child, leaving a zombie and a shell prompt fighting it for the
  // terminal.
  int status;
  while (waitpid(pid, &status, 0) < 0 && errno == EINTR)
    ;                               // interrupted, child still ours: keep waiting
  return 0;
}

static int split_line(char *line, char **out, int max);   // defined below

// --- ONE LINE OF SHELL ---
// Tokenise `line` and run it: a builtin if the first word names one, otherwise
// an external program. Returns 1 if the line asked the shell to exit.
//
// Exists so the REPL and the .tetrishrc reader below share one dispatch path.
// Two copies of "builtin, else exec it" is two things to keep in step, and a
// startup file that behaves differently from the prompt is a surprise, not a
// feature.
//
// `line` is modified in place by the tokenizer, so callers must pass a
// writable buffer, not a string literal.
static int run_line(char *line){
  char *cmdv[MAX_ARGS];
  int n = split_line(line, cmdv, MAX_ARGS);
  if (n == 0)                       // blank or whitespace-only
    return 0;

  for (int i = 0; i < N_BUILTINS; i++){
    if (strcmp(cmdv[0], names[i]) == 0)
      return funcs[i](cmdv);        // cmd_exit returns 1; the rest return 0
  }
  launch(cmdv);                     // not a builtin: run it as a program
  return 0;
}

// --- .tetrishrc EXECUTION ---
// Run the rc file the way a shell runs its startup file: every line, in order,
// through the same dispatch the prompt uses.
//
// The file is shared with the daemons, which read the same lines as
// configuration. Lines rc_load already consumed as directives are skipped
// (rc_is_directive); everything else is executed. That is what lets a user put
//
//     dspawn bin/tetrislogd .tetrishrc
//     dspawn bin/tetrisd .tetrishrc
//
// in the rc file next to listen_port and have the whole system come up on
// `tetrish` alone, without the shell trying to exec "listen_port" and printing
// a not-found error for every directive in the file.
//
// A missing rc file is not an error here. rc_load has already run and would
// have refused to start if the file were unreadable.
static void run_rc_file(const char *path){
  FILE *fp = fopen(path, "r");
  if (fp == NULL) return;

  char line[512];
  while (fgets(line, sizeof line, fp) != NULL){
    line[strcspn(line, "#\n")] = '\0';    // strip a trailing comment and the newline

    // Peek at the first word without touching the line: split_line writes NULs
    // into it, and we may still want to hand the whole line over.
    char peek[512];
    snprintf(peek, sizeof peek, "%s", line);
    char *first = strtok(peek, " \t");
    if (first == NULL) continue;          // blank line
    if (rc_is_directive(first)) continue; // configuration, not a command

    run_line(line);                       // exit from the rc file is ignored on purpose:
                                          // an `exit` there would close the shell before
                                          // the user ever saw a prompt.
  }
  fclose(fp);
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
  // Handler for SIGINT, the signal Ctrl+C sends. sigaction with sa_flags 0, so
  // no SA_RESTART, and that is the whole point.
  // The older signal() turns SA_RESTART on behind your back. With it on, a
  // blocked fgets gets quietly restarted after the handler runs, so Ctrl+C
  // looked like it did nothing and the "Interrupted by user" branch below could
  // never fire. With sa_flags 0, fgets returns NULL with errno EINTR instead,
  // the loop sees the interrupt and prints a fresh prompt.
  // (Same reason tetrislogd sets its recvfrom up this way.)
  struct sigaction sa_int;
  memset(&sa_int, 0, sizeof sa_int);
  sa_int.sa_handler = handle_signal;
  sigemptyset(&sa_int.sa_mask);
  sa_int.sa_flags = 0;
  sigaction(SIGINT, &sa_int, NULL);

  // SIGCHLD arrives whenever one of our children changes state, which is how we
  // find out a background job finished. sigaction again, so the flags are
  // written out where you can see them.
  // This one DOES want SA_RESTART: a SIGCHLD landing while the user is midway
  // through typing must not cut off their fgets. The cost is that the finished
  // job gets cleaned up a little later, just before the next prompt, instead of
  // instantly.
  struct sigaction sa;
  memset(&sa, 0, sizeof sa);
  sa.sa_handler = handle_sigchld;
  sigemptyset(&sa.sa_mask);
  sa.sa_flags = SA_RESTART;
  sigaction(SIGCHLD, &sa, NULL);

  // Config file, named on the command line or .tetrishrc by default.
  // Loaded BEFORE the banner, so bad config refuses to start instead of
  // printing a prompt and pretending everything is fine.
  const char *rc_path = (argc > 1) ? argv[1] : ".tetrishrc";
  Config config;
  if (rc_load(rc_path, &config) != 0){
    fprintf(stderr, "Failed to load configuration from %s\n", rc_path);
    return 1;
  }

  printf("Tetrish REPL - Type 'exit' or Ctrl + C to exit\n");

  // Run the rc file before the first prompt, the way a shell sources its
  // startup file. rc_load above already read this same file for its config
  // directives; this pass runs the lines that are commands.
  run_rc_file(rc_path);

  // The REPL itself
  while (1){
    // A background child that finished while we were busy gets cleaned up
    // here, before the prompt, so no zombie survives past it.
    if (child_exited)
      reap_jobs();

    // fflush or the prompt sits in the buffer until after the user types
    printf("tetrish> ");
    fflush(stdout);

    interrupted = 0;

    // Ctrl+D exits the program, Ctrl+C goes back to the prompt.
    // fgets returning NULL is either of those, or a real error.
    if (fgets(input, sizeof(input), stdin) == NULL){
      if (feof(stdin)) {
        printf("\nEOF received, exiting....\n");
        break;
      } else if (interrupted){
        printf("\nInterrupted by user\n");
        continue;
      } else {
      // something else went wrong
      printf("\nError reading input\n");
      continue;
    }
    }
    // fgets keeps the newline; cut it off
    input[strcspn(input, "\n")] = '\0';

    // Builtin if the first word names one, otherwise an external program. Same
    // function the rc file goes through, so prompt and startup file cannot
    // drift apart.
    if (run_line(input))        // cmd_exit asked us to quit
      break;
  }
  return 0;
}
