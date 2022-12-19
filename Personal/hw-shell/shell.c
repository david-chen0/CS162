#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <signal.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

#include "tokenizer.h"

/* Convenience macro to silence compiler warnings about unused function parameters. */
#define unused __attribute__((unused))

/* Whether the shell is connected to an actual terminal or not. */
bool shell_is_interactive;

/* File descriptor for the shell input */
int shell_terminal;

/* Terminal mode settings for the shell */
struct termios shell_tmodes;

/* Process group id for the shell */
pid_t shell_pgid;

int cmd_help(struct tokens* tokens);
int cmd_exit(struct tokens* tokens);
int cmd_pwd(struct tokens* tokens);
int cmd_cd(struct tokens* tokens);

int MAX_STR_LEN = 162; // this might not be long enough, just using a random int for length

/* Built-in command functions take token array (see parse.h) and return int */
typedef int cmd_fun_t(struct tokens* tokens);

/* Built-in command struct and lookup table */
typedef struct fun_desc {
  cmd_fun_t* fun;
  char* cmd;
  char* doc;
} fun_desc_t;

fun_desc_t cmd_table[] = {
    {cmd_help, "?", "show this help menu"},
    {cmd_exit, "exit", "exit the command shell"},
    {cmd_pwd, "pwd", "print the current working directory"},
    {cmd_cd, "cd", "change the current working directory"},
};

/* Prints a helpful description for the given command */
int cmd_help(unused struct tokens* tokens) {
  for (unsigned int i = 0; i < sizeof(cmd_table) / sizeof(fun_desc_t); i++)
    printf("%s - %s\n", cmd_table[i].cmd, cmd_table[i].doc);
  return 1;
}

/* Exits this shell */
int cmd_exit(unused struct tokens* tokens) { exit(0); }

/* Prints the current working directory */
int cmd_pwd(unused struct tokens* tokens) {
  char* buf = malloc(MAX_STR_LEN);
  getcwd(buf, MAX_STR_LEN);
  printf("%s\n", buf);

  // tokens_destroy(tokens);
  return 1; // might have to return something else, just copying from cmd_help rn
}

/* Changes the current working directory to the input directory */
int cmd_cd(unused struct tokens* tokens) {
  char* inputDir = tokens_get_token(tokens, 1);
  int status = chdir(inputDir);

  // tokens_destroy(tokens);
  if (status == -1) {
    return -1; // assuming this means error
  }
  return 1; // might have to return something else, just copying from cmd_help rn
}

/* Looks up the built-in command, if it exists. */
int lookup(char cmd[]) {
  for (unsigned int i = 0; i < sizeof(cmd_table) / sizeof(fun_desc_t); i++)
    if (cmd && (strcmp(cmd_table[i].cmd, cmd) == 0))
      return i;
  return -1;
}

/* Intialization procedures for this shell */
void init_shell() {
  /* Our shell is connected to standard input. */
  shell_terminal = STDIN_FILENO;

  /* Check if we are running interactively */
  shell_is_interactive = isatty(shell_terminal);

  if (shell_is_interactive) {
    /* If the shell is not currently in the foreground, we must pause the shell until it becomes a
     * foreground process. We use SIGTTIN to pause the shell. When the shell gets moved to the
     * foreground, we'll receive a SIGCONT. */
    while (tcgetpgrp(shell_terminal) != (shell_pgid = getpgrp()))
      kill(-shell_pgid, SIGTTIN);

    // this should ignore the signals we need to handle
    signal(SIGINT, SIG_IGN);
    signal(SIGQUIT, SIG_IGN);
    signal(SIGKILL, SIG_IGN);
    signal(SIGTERM, SIG_IGN);
    signal(SIGTSTP, SIG_IGN);
    signal(SIGCONT, SIG_IGN);
    signal(SIGTTIN, SIG_IGN);
    signal(SIGTTOU, SIG_IGN);

    /* Saves the shell's process id */
    shell_pgid = getpid();

    /* Take control of the terminal */
    tcsetpgrp(shell_terminal, shell_pgid);

    /* Save the current termios to a variable, so it can be restored later. */
    tcgetattr(shell_terminal, &shell_tmodes);
  }
}

/* Handles Path Resolution */
char* pathResolution(char* pathEnd) {
  char* path = getenv("PATH");
  if (access(pathEnd, F_OK) == 0) {
    return pathEnd;
  }

  char slash = '/';
  char delimiter = ':';
  char* curPath;
  char* token = strtok(path, &delimiter);

  while (token) {
    curPath = malloc(MAX_STR_LEN * sizeof(char));
    if (curPath == NULL) {
      return NULL;
    }

    strcat(curPath, token);
    strncat(curPath, &slash, 1); // need to limit length cuz otherwise it'll keep going up stack and read the delim
    strcat(curPath, pathEnd);

    if (access(curPath, F_OK) == 0) { // absolute path exists
      return curPath;
    }

    token = strtok(0, &delimiter);
  }
  return NULL;
}

/* Handles redirection */
// Returns -1 if error
int redirect(char** execArgv) {
  int argLength = 0;
  for (; execArgv[argLength] != NULL; argLength++) { }


  int directIn = 0; // for the < redirection
  int directOut = 0; // for the > redirection
  for (int i = 0; i < argLength; i++) {
    if (strcmp(execArgv[i], "<") == 0) {
      directIn = i;
    } else if (strcmp(execArgv[i], ">") == 0) {
      directOut = i;
    }
  }


  if (directIn > 0) { // redirect file contents to stdin
    if (execArgv[directIn + 1] == NULL) {
      return -1;
    }

    char* fileName = execArgv[directIn + 1];
    int infileDescriptor = open(fileName, O_RDONLY);

    if (directOut > directIn) {
      directOut -= 2;
    }

    // deleting the < and filename from execArgv so that the right thing will be proessed later
    for (; directIn < argLength - 2; directIn++) {
      free(execArgv[directIn]);
      execArgv[directIn] = execArgv[directIn + 2];
      execArgv[directIn + 2] = NULL;
    }
    free(execArgv[argLength - 2]);
    free(execArgv[argLength - 1]);
    execArgv[directIn] = NULL;

    argLength -= 2;

    dup2(infileDescriptor, STDIN_FILENO); // changes stdin(should be fd=0) to infile
    close(infileDescriptor);
  }

  if (directOut > 0) { // redirect process's stdout to the file
    if (execArgv[directOut + 1] == NULL) {
      return -1;
    }

    char* fileName = execArgv[directOut + 1];
    mode_t mode = S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH;
    int outfileDescriptor = open(fileName, O_CREAT | O_WRONLY | O_TRUNC, mode);

    // deleting the < and filename from execArgv so that the right thing will be proessed later
    for (; directOut < argLength - 2; directOut++) {
      free(execArgv[directOut]);
      execArgv[directOut] = execArgv[directOut + 2];
      execArgv[directOut + 2] = NULL;
    }
    execArgv[directOut] = NULL;
    free(execArgv[argLength - 2]);
    free(execArgv[argLength - 1]);

    argLength -= 2;

    dup2(outfileDescriptor, STDOUT_FILENO); // changes stdout(should be fd=1) to outfile
    close(outfileDescriptor);
  }
  return 0;
}

int main(unused int argc, unused char* argv[]) {
  init_shell();

  static char line[4096];
  int line_num = 0;

  /* Please only print shell prompts when standard input is not a tty */
  if (shell_is_interactive)
    fprintf(stdout, "%d: ", line_num);

  while (fgets(line, 4096, stdin)) {
    /* Split our line into words. */
    struct tokens* tokens = tokenize(line);

    /* Find which built-in function to run. */
    int fundex = lookup(tokens_get_token(tokens, 0));

    if (fundex >= 0) {
      cmd_table[fundex].fun(tokens);
    } else {
      int status = 0;
      pid_t pid = fork();

      if (pid == 0) { // child
        size_t numWords = tokens_get_length(tokens);

        char** execArgv = malloc(numWords * sizeof(char*));
        if (execArgv == NULL) {
          return -1;
        }

        int pipeLocations[numWords];
        int numPipeProcesses = 0;

        for (int i = 0; i < numWords; i++) {
          execArgv[i] = malloc(MAX_STR_LEN * sizeof(char));
          if (execArgv[i] == NULL) {
            return -1;
          }

          strcpy(execArgv[i], tokens_get_token(tokens, i));
          
          if (strcmp(execArgv[i], "|") == 0) {
            pipeLocations[numPipeProcesses] = i;
            numPipeProcesses++;
          }
        }
        execArgv[numWords] = NULL;

        int argStart = 0;
        int prevFD = -1;
        int pipefd[2];
        pid_t childPID;
        for (int i = 0; i < numPipeProcesses; i++) {
          int pipeLocation = pipeLocations[i];
          execArgv[pipeLocation] = NULL;

          pipe(pipefd);

          int subprocessStatus = 0;
          childPID = fork();
          if (childPID == -1) {
            return -1;
          } else if (childPID == 0) { // child
            close(pipefd[0]);
            if (prevFD != -1) { // not first time, so we need to read from prev pipe
              dup2(prevFD, STDIN_FILENO); // should be fd=0 for stdin
              close(prevFD);
            }

            dup2(pipefd[1], STDOUT_FILENO); // should be fd=1 for stdout
            close(pipefd[1]);

            break;
          } else { // parent
            close(pipefd[1]);
            wait(&subprocessStatus);
            prevFD = pipefd[0];

            argStart = pipeLocation + 1;

            if (i == numPipeProcesses - 1) { // for the last execution of pipes
              dup2(prevFD, STDIN_FILENO);
              close(prevFD);
            }
          }
        }

        // Redirection
        if (redirect(&execArgv[argStart]) == -1) {
          return -1;
        }

        // Path Resolution
        char* absolutePath = pathResolution(execArgv[argStart]);
        if (absolutePath == NULL) {
          return -1;
        }

        // this should set the signals we need to handle back to default
        signal(SIGINT, SIG_DFL);
        signal(SIGQUIT, SIG_DFL);
        signal(SIGKILL, SIG_DFL);
        signal(SIGTERM, SIG_DFL);
        signal(SIGTSTP, SIG_DFL);
        signal(SIGCONT, SIG_DFL);
        signal(SIGTTIN, SIG_DFL);
        signal(SIGTTOU, SIG_DFL);

        strcpy(execArgv[argStart], absolutePath);
        execv(execArgv[argStart], &execArgv[argStart]);
      } else { // parent
        wait(&status);
      }
    }

    if (shell_is_interactive)
      /* Please only print shell prompts when standard input is not a tty */
      fprintf(stdout, "%d: ", ++line_num);

    /* Clean up memory */
    tokens_destroy(tokens);
  }

  return 0;
}
