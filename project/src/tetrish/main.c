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
// We keep track of every background program that we start with dspawn.
// To do that, we store them in a linked list, where each item in the
// list is one background job.
// Only the main loop is allowed to read or change this list. The signal
// handler that runs when a background child finishes does not touch the
// list at all. It only sets a flag. We keep the handler this small on
// purpose, because a signal handler is not allowed to safely call things
// like malloc or printf.
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

// Clean up after every background child that has finished. This is the step
// that stops finished children from staying around as zombie processes.
// We pass WNOHANG to waitpid, which means "return straight away instead of
// waiting", so this never holds up the prompt. A return value of 0 means that
// child is still running.
// We call waitpid once for each specific pid, and never with -1. If we used
// -1 here, waitpid could accidentally collect a foreground child that launch()
// is currently waiting for, and then launch() would lose track of it.
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

// dspawn CMD [ARGS...]: start CMD as a background job. This uses the same fork
// and execvp steps as launch(), but with two differences. First, the parent
// does not wait for the child. It just records the child's pid and goes back to
// the prompt. Second, the child is moved into its own process group, so that a
// Ctrl+C meant for the shell's foreground work cannot reach and kill the
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

// dcheck PID: check on one job without waiting. If it is still running we say
// so. If it has finished we report its exit code, and then we remove the job
// from the list, because we only report a finished job once.
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
      // Take this job out of the list. If it had a job before it, point that
      // one past it. Otherwise it was the first job, so move the head forward.
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
  // The EINTR loop matters now that SIGINT has NO SA_RESTART: Ctrl+C aimed
  // at the foreground child interrupts OUR waitpid too. Without the retry
  // we would abandon the child (zombie + shell prompt fighting the child
  // for the terminal).
  int status;
  while (waitpid(pid, &status, 0) < 0 && errno == EINTR)
    ;                               // interrupted, child still ours: keep waiting
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
  // Set up the handler for SIGINT, which is the signal that Ctrl+C sends. We
  // use sigaction with sa_flags set to 0, which means we deliberately do not
  // ask for SA_RESTART.
  // Here is why that matters. The older signal() function turns SA_RESTART on
  // for you behind the scenes. With SA_RESTART on, a blocked fgets is quietly
  // restarted after the handler runs, so pressing Ctrl+C looked like it did
  // nothing, and the "Interrupted by user" branch further down could never
  // run. With sa_flags set to 0, fgets instead returns NULL and sets errno to
  // EINTR, so the loop notices the interrupt and can print a fresh prompt.
  // (This is the same reason tetrislogd sets up its recvfrom the same way.)
  struct sigaction sa_int;
  memset(&sa_int, 0, sizeof sa_int);
  sa_int.sa_handler = handle_signal;
  sigemptyset(&sa_int.sa_mask);
  sa_int.sa_flags = 0;
  sigaction(SIGINT, &sa_int, NULL);

  // The system sends SIGCHLD whenever any of our children changes state, and
  // that is how we find out that a background job has finished. We use
  // sigaction instead of signal() so that the flags are written out clearly.
  // This time we do want SA_RESTART, because if a SIGCHLD arrives while the
  // user is in the middle of typing, we do not want it to cut off the fgets
  // they are using. The trade-off is that we clean the finished job up a little
  // later, right before the next prompt, instead of instantly.
  struct sigaction sa;
  memset(&sa, 0, sizeof sa);
  sa.sa_handler = handle_sigchld;
  sigemptyset(&sa.sa_mask);
  sa.sa_flags = SA_RESTART;
  sigaction(SIGCHLD, &sa, NULL);

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
    // If a background child finished while we were busy, clean it up now,
    // before we print the prompt, so that no zombie process is left waiting
    // past the next prompt.
    if (child_exited)
      reap_jobs();

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
