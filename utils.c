#include "utils.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/wait.h>

#define TOKEN_DELIMITERS " \n" // space and newline are the only delimiters

/*
 * Parses the input line into a NULL-terminated array of tokens (arguments)
 * input is the raw command line string from the user
 * function returns a dynamically allocated array of strings
*/
char** parse_command(char* input) {

    int bufsize = 64; // initial array size for the list of arguments
    int position = 0;
    char** tokens = malloc(bufsize * sizeof(char*));
    char* token;
    if (!tokens) {
        fprintf(stderr, "myshell: allocation error\n");
        exit(EXIT_FAILURE);
    }

    // use strtok to split the input string into tokens
    token = strtok(input, TOKEN_DELIMITERS);
    while (token != NULL) {
        tokens[position] = token;
        position++;
        // if we've exceeded the buffer, reallocate more memory
        if (position >= bufsize) {
            bufsize += 64;
            tokens = realloc(tokens, bufsize * sizeof(char*));
            if (!tokens) {
                fprintf(stderr, "myshell: allocation error\n");
                exit(EXIT_FAILURE);
            }
        }
        // get the next token
        token = strtok(NULL, TOKEN_DELIMITERS);
    }

    // make sure the last element of the array is NULL for execvp
    tokens[position] = NULL;
    return tokens;
}

/*
 * Parses the redirs from an array of tokens
 * input is the array of tokens, a generic Redirs struct, and an empty err message
 * function returns 0 on success and - 1 if a command is missing
*/
int parse_redirs(char **args, Redirs *R, char **errmsg) {
    R->in_file = NULL;
    R->out_file = NULL;
    R->err_file = NULL;

    int write_idx = 0; // index where we write the next non-redirection argument
    for (int i = 0; args[i] != NULL; i++) {
        if (strcmp(args[i], "<") == 0) {
            if (R->in_file) { *errmsg = "Duplicate input redirection."; return -1; }
            if (!args[i+1]) { *errmsg = "bash: syntax error near unexpected token `newline'"; return -1; } // input file not specified
            R->in_file = args[i+1];
            i++; // skip filename
            continue;
        }
        if (strcmp(args[i], ">") == 0) {
            if (R->out_file) { *errmsg = "Duplicate output redirection."; return -1; }
            if (!args[i+1]) { *errmsg = "bash: syntax error near unexpected token `newline'"; return -1; } // output file not specified
            R->out_file = args[i+1];
            i++; // skip filename
            continue;
        }
        if (strcmp(args[i], "2>") == 0) {
            if (R->err_file) { *errmsg = "Duplicate error redirection."; return -1; }
            if (!args[i+1]) { *errmsg = "bash: syntax error near unexpected token `newline'"; return -1; } // error output file not specified
            R->err_file = args[i+1];
            i++; // skip filename
            continue;
        }
        // keep normal argv tokens
        args[write_idx++] = args[i];
    }
    args[write_idx] = NULL;
    if (!args[0]) { *errmsg = "Command missing."; return -1; }
    return 0;
}

// Returns the number of non-NULL entries in the argv array `t`.
static int count_tokens(char **t) { 
    int n=0; 
    while (t && t[n]) { // Uses short-circuiting (`t && t[n]`) so:
        n++;            //   - If `t` is NULL, we don't dereference it.
    }                   //   - We stop before the trailing NULL terminator.
    return n;
}

/*
 * Builds pipeline stages from a flat token list separated by '|'.
 * input are tokens (NULL-terminated), stages_out is the 2-d array of Stage structs, 
   and nstages_out is the number of stages in the entire command
 * function returns number of stages (>=1) on success, -1 on error (errmsg set)
*/
int build_pipeline(char **tokens, Stage **stages_out, int *nstages_out, const char **errmsg) {
    *errmsg = NULL;
    *stages_out = NULL;
    *nstages_out = 0;

    const int ntok = count_tokens(tokens);
    if (ntok == 0) {
        *errmsg = "Command missing.";
        return -1;
    }

    // first pass: count stages and validate that there are no leading/trailing/double pipes.
    int stages = 1;
    for (int i = 0; i < ntok; i++) {
        if (strcmp(tokens[i], "|") == 0) {
            if (i == 0) { *errmsg = "Command missing after pipe."; return -1; }  // leading '|'
            if (i == ntok - 1) { *errmsg = "Command missing after pipe."; return -1; }  // trailing '|'
            if (i+1 < ntok && strcmp(tokens[i+1], "|") == 0) { *errmsg = "bash: syntax error near unexpected token `|'"; return -1; } // double or more '|'
            stages++;
        }
    }

    Stage *S = calloc(stages, sizeof(Stage));
    if (!S) { *errmsg = "Internal error: OOM."; return -1; }

    // second pass: slice tokens per stage, then parse redirs for each slice and compact argv 
    //              (contains no redirections or file names) in place.
    int sidx = 0;
    int start = 0;
    for (int i = 0; i <= ntok; i++) {
        if (i == ntok || strcmp(tokens[i], "|") == 0) {
            // temporarily NULL-terminate this stage
            char *saved = NULL;
            if (i < ntok) {
                saved = tokens[i]; 
                tokens[i] = NULL;
            }

            // parse redirections and compact argv in-place for this stage
            char *perr = NULL;
            if (parse_redirs(&tokens[start], &S[sidx].r, &perr) < 0) {
                *errmsg = perr; // e.g., "bash: syntax error near unexpected token `newline'", etc.
                // restore and cleanup
                if (saved) tokens[i] = saved;
                free(S);
                return -1;
            }

            // when a pipeline stage has no actual command left after redirections are stripped
            // for example, "cat | > out.txt"
            S[sidx].argv = &tokens[start];
            if (!S[sidx].argv[0]) {
                *errmsg = "Command missing in pipe sequence.";
                if (saved) tokens[i] = saved;
                free(S);
                return -1;
            }

            // restore token for next scan
            if (saved) tokens[i] = saved;
            start = i + 1;
            sidx++;
        }
    }

    *stages_out = S;
    *nstages_out = stages;
    return stages;
}

/*
 * Executes an n-stage pipeline (S[0..n-1]) using fork/pipe/dup2.
 * input is S (stages with argv + redirs) and n (# of stages)
 * function returns 0 on success, -1 on immediate setup failure (e.g., pipe/fork OOM)
*/
int exec_pipeline(Stage *S, int n) {
    // if just one stage -> no pipes, only redirs + exec
    if (n == 1) {
        pid_t pid = fork();
        if (pid < 0) { perror("fork"); return -1; }
        if (pid == 0) {
            // child: apply redirs then exec
            int fd;
            if (S[0].r.in_file) { // if input file redirection exists
                fd = open(S[0].r.in_file, O_RDONLY);
                if (fd < 0) { perror(S[0].r.in_file); _exit(1); }
                if (dup2(fd, STDIN_FILENO) < 0) { perror("dup2 <"); _exit(1); } // redirect standard input to the file
                close(fd);
            }
            if (S[0].r.out_file) { // if output file redirection exists
                fd = open(S[0].r.out_file, O_WRONLY|O_CREAT|O_TRUNC, 0644);
                if (fd < 0) { perror(S[0].r.out_file); _exit(1); }
                if (dup2(fd, STDOUT_FILENO) < 0) { perror("dup2 >"); _exit(1); }
                close(fd);
            }
            if (S[0].r.err_file) { // if error file redirection exists
                fd = open(S[0].r.err_file, O_WRONLY|O_CREAT|O_TRUNC, 0644);
                if (fd < 0) { perror(S[0].r.err_file); _exit(1); }
                if (dup2(fd, STDERR_FILENO) < 0) { perror("dup2 2>"); _exit(1); }
                close(fd);
            }
            if (execvp(S[0].argv[0], S[0].argv) == -1) {
                if (errno == ENOENT) {
                    if (S[0].argv[0][0] == '.' && S[0].argv[0][1] == '/') {             // in order to immitate bash behavior, we check if 
                        fprintf(stderr, "%s: No such file or directory\n", S[0].argv[0]); // the executable exists, so we can give identical err msg
                    }
                    else {
                        fprintf(stderr, "%s: command not found\n", S[0].argv[0]); // otherwise exec failed cos command doesn't exist
                    }
                }
                else {
                    // for all other errors, print the default system error
                    fprintf(stderr, "%s: %s\n", S[0].argv[0], strerror(errno));
                }
                exit(EXIT_FAILURE);
            }
        }
        int status;
        waitpid(pid, &status, 0);
        return 0;
    }

    // n >= 2, 2 or more stages in a single command
    int (*pfds)[2] = calloc(n-1, sizeof(int[2])); // n stages need n-1 pipes
    if (!pfds) { perror("calloc"); return -1; }

    for (int i = 0; i < n-1; i++) {
        if (pipe(pfds[i]) < 0) { perror("pipe"); free(pfds); return -1; }
    }

    pid_t *pids = calloc(n, sizeof(pid_t));
    if (!pids) { perror("calloc"); free(pfds); return -1; }

    for (int i = 0; i < n; i++) { // for each stage
        pids[i] = fork();
        if (pids[i] < 0) {
            perror("fork");
            // cleanup: close all pipes and wait for already-spawned children
            for (int k = 0; k < n-1; k++) { close(pfds[k][0]); close(pfds[k][1]); }
            for (int k = 0; k < i; k++) waitpid(pids[k], NULL, 0);
            free(pfds); free(pids);
            return -1;
        }

        if (pids[i] == 0) {
            // ---- child i ----
            // connect stdin/stdout to pipes where appropriate
            if (i > 0) { // not first stage: read end from previous pipe
                if (dup2(pfds[i-1][0], STDIN_FILENO) < 0) { perror("dup2 in"); _exit(1); }
            }
            if (i < n-1) { // not last stage: write end to next pipe
                if (dup2(pfds[i][1], STDOUT_FILENO) < 0) { perror("dup2 out"); _exit(1); }
            }

            // close all pipe fds (we only need the duped ends)
            for (int k = 0; k < n-1; k++) {
                close(pfds[k][0]);
                close(pfds[k][1]);
            }

            // apply redirections for this stage. (exaclty the same as we did in the case n==1)
            // we apply redirs AFTER pipe dup2s so redirs override pipeline if both are present.
            int fd;
            if (S[i].r.in_file) {
                fd = open(S[i].r.in_file, O_RDONLY);
                if (fd < 0) { perror(S[i].r.in_file); _exit(1); }
                if (dup2(fd, STDIN_FILENO) < 0) { perror("dup2 <"); _exit(1); }
                close(fd);
            }
            if (S[i].r.out_file) {
                fd = open(S[i].r.out_file, O_WRONLY|O_CREAT|O_TRUNC, 0644);
                if (fd < 0) { perror(S[i].r.out_file); _exit(1); }
                if (dup2(fd, STDOUT_FILENO) < 0) { perror("dup2 >"); _exit(1); }
                close(fd);
            }
            if (S[i].r.err_file) {
                fd = open(S[i].r.err_file, O_WRONLY|O_CREAT|O_TRUNC, 0644);
                if (fd < 0) { perror(S[i].r.err_file); _exit(1); }
                if (dup2(fd, STDERR_FILENO) < 0) { perror("dup2 2>"); _exit(1); }
                close(fd);
            }

            // exec the stage
            if (execvp(S[i].argv[0], S[i].argv) == -1) {
                if (errno == ENOENT) {
                    if (S[i].argv[0][0] == '.' && S[i].argv[0][1] == '/') {               // in order to immitate bash behavior, we check if 
                        fprintf(stderr, "%s: No such file or directory\n", S[i].argv[0]); // the executable exists, so we can give identical err msg
                    }
                    else {
                        fprintf(stderr, "%s: command not found\n", S[i].argv[0]); // otherwise exec failed cos command doesn't exist
                    }
                }
                else {
                    // For all other errors, print the default system error
                    fprintf(stderr, "%s: %s\n", S[i].argv[0], strerror(errno));
                }
                exit(EXIT_FAILURE);
            }
        }
    }

    // parent closes all pipe fds
    for (int k = 0; k < n-1; k++) { close(pfds[k][0]); close(pfds[k][1]); }
    free(pfds);
    int status;
    for (int i = 0; i < n; i++) {
        waitpid(pids[i], &status, 0);  // make sure no zombies
    }
    free(pids);
    return 0;
}