#include "utils.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/wait.h>

#define TOKEN_DELIMITERS " \n" // space and newline are the only delimiters

/**
 * Parses the input line into a NULL-terminated array of tokens (arguments).
 * @param input The raw command line string from the user.
 * @return A dynamically allocated array of strings. The last element is NULL.
 */

char** parse_command(char* input) {

    int bufsize = 64; // initial array size for the list of arguments
    int position = 0;

    // Allocate memory for an array of character pointers (strings)
    char** tokens = malloc(bufsize * sizeof(char*));
    char* token;

    if (!tokens) {
        fprintf(stderr, "myshell: allocation error\n");
        exit(EXIT_FAILURE);
    }

    // Use strtok to split the input string into tokens
    token = strtok(input, TOKEN_DELIMITERS);

    while (token != NULL) {
        tokens[position] = token;
        position++;

        // If we've exceeded the buffer, reallocate more memory
        if (position >= bufsize) {
            bufsize += 64;
            tokens = realloc(tokens, bufsize * sizeof(char*));
            if (!tokens) {
                fprintf(stderr, "myshell: allocation error\n");
                exit(EXIT_FAILURE);
            }
        }

        token = strtok(NULL, TOKEN_DELIMITERS);
    }

    // The last element of the array must be NULL for execvp
    tokens[position] = NULL;
    return tokens;
}

int parse_redirs(char **args, Redirs *R, char **errmsg) {
    R->in_file = NULL;
    R->out_file = NULL;
    R->err_file = NULL;

    int write_idx = 0;
    for (int i = 0; args[i] != NULL; i++) {
        if (strcmp(args[i], "<") == 0) {
            if (R->in_file) { *errmsg = "Duplicate input redirection."; return -1; }
            if (!args[i+1]) { *errmsg = "Input file not specified."; return -1; }
            R->in_file = args[i+1];
            i++; // skip filename
            continue;
        }
        if (strcmp(args[i], ">") == 0) {
            if (R->out_file) { *errmsg = "Duplicate output redirection."; return -1; }
            if (!args[i+1]) { *errmsg = "Output file not specified."; return -1; }
            R->out_file = args[i+1];
            i++;
            continue;
        }
        if (strcmp(args[i], "2>") == 0) {
            if (R->err_file) { *errmsg = "Duplicate error redirection."; return -1; }
            if (!args[i+1]) { *errmsg = "Error output file not specified."; return -1; }
            R->err_file = args[i+1];
            i++;
            continue;
        }
        // keep normal argv tokens
        args[write_idx++] = args[i];
    }
    args[write_idx] = NULL;

    if (!args[0]) { *errmsg = "Command missing."; return -1; }
    return 0;
}

static int count_tokens(char **t) { 
    int n=0; 
    while (t && t[n]) {
        n++;    
    } 
    return n;
}

int build_pipeline(char **tokens, Stage **stages_out, int *nstages_out, const char **errmsg) {
    *errmsg = NULL;
    *stages_out = NULL;
    *nstages_out = 0;

    const int ntok = count_tokens(tokens);
    if (ntok == 0) {
        *errmsg = "Command missing.";
        return -1;
    }

    // First pass: count stages and validate that there are no leading/trailing/double pipes.
    int stages = 1;
    for (int i = 0; i < ntok; i++) {
        if (strcmp(tokens[i], "|") == 0) {
            if (i == 0) { *errmsg = "Command missing after pipe."; return -1; }  // leading '|'
            if (i == ntok - 1) { *errmsg = "Command missing after pipe."; return -1; }  // trailing '|'
            if (i+1 < ntok && strcmp(tokens[i+1], "|") == 0) { *errmsg = "Empty command between pipes."; return -1; }
            stages++;
        }
    }

    Stage *S = calloc(stages, sizeof(Stage));
    if (!S) { *errmsg = "Internal error: OOM."; return -1; }

    // Second pass: slice tokens per stage, then parse redirs for each slice and compact argv in place.
    int sidx = 0;
    int start = 0;
    for (int i = 0; i <= ntok; i++) {
        if (i == ntok || strcmp(tokens[i], "|") == 0) {
            // Temporarily NULL-terminate this stage
            char *saved = NULL;
            if (i < ntok) {
                saved = tokens[i]; 
                tokens[i] = NULL;
            }

            // Parse redirections and compact argv in-place for this stage
            char *perr = NULL;
            if (parse_redirs(&tokens[start], &S[sidx].r, &perr) < 0) {
                *errmsg = perr; // e.g., "Input file not specified.", etc.
                // restore and cleanup
                if (saved) tokens[i] = saved;
                free(S);
                return -1;
            }
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

int exec_pipeline(Stage *S, int n) {
    if (n <= 0) return 0;

    // special fast path: just one stage -> no pipes, only redirs + exec
    if (n == 1) {
        pid_t pid = fork();
        if (pid < 0) { perror("fork"); return -1; }
        if (pid == 0) {
            // child: apply redirs then exec
            int fd;
            if (S[0].r.in_file) {
                fd = open(S[0].r.in_file, O_RDONLY);
                if (fd < 0) { perror(S[0].r.in_file); _exit(1); }
                if (dup2(fd, STDIN_FILENO) < 0) { perror("dup2 <"); _exit(1); }
                close(fd);
            }
            if (S[0].r.out_file) {
                fd = open(S[0].r.out_file, O_WRONLY|O_CREAT|O_TRUNC, 0644);
                if (fd < 0) { perror(S[0].r.out_file); _exit(1); }
                if (dup2(fd, STDOUT_FILENO) < 0) { perror("dup2 >"); _exit(1); }
                close(fd);
            }
            if (S[0].r.err_file) {
                fd = open(S[0].r.err_file, O_WRONLY|O_CREAT|O_TRUNC, 0644);
                if (fd < 0) { perror(S[0].r.err_file); _exit(1); }
                if (dup2(fd, STDERR_FILENO) < 0) { perror("dup2 2>"); _exit(1); }
                close(fd);
            }
            execvp(S[0].argv[0], S[0].argv);
            perror(S[0].argv[0]);
            _exit(127);
        }
        // Spec says you must wait for the last process to finish before prompting again.
        // We wait() for the only child (and you *should* reap to avoid zombies). :contentReference[oaicite:5]{index=5}
        int status;
        waitpid(pid, &status, 0);
        return 0;
    }

    // n >= 2
    int (*pfds)[2] = calloc(n-1, sizeof(int[2]));
    if (!pfds) { perror("calloc"); return -1; }

    for (int i = 0; i < n-1; i++) {
        if (pipe(pfds[i]) < 0) { perror("pipe"); free(pfds); return -1; }
    }

    pid_t *pids = calloc(n, sizeof(pid_t));
    if (!pids) { perror("calloc"); free(pfds); return -1; }

    for (int i = 0; i < n; i++) {
        pids[i] = fork();
        if (pids[i] < 0) {
            perror("fork");
            // Parent cleanup: close all pfds and wait for already-spawned children
            for (int k = 0; k < n-1; k++) { close(pfds[k][0]); close(pfds[k][1]); }
            for (int k = 0; k < i; k++) waitpid(pids[k], NULL, 0);
            free(pfds); free(pids);
            return -1;
        }

        if (pids[i] == 0) {
            // ---- child i ----
            // 1) Connect stdin/stdout to pipes where appropriate
            if (i > 0) { // not first: read end from previous pipe
                if (dup2(pfds[i-1][0], STDIN_FILENO) < 0) { perror("dup2 in"); _exit(1); }
            }
            if (i < n-1) { // not last: write end to next pipe
                if (dup2(pfds[i][1], STDOUT_FILENO) < 0) { perror("dup2 out"); _exit(1); }
            }

            // 2) Close all pipe fds (we only need the duped ends)
            for (int k = 0; k < n-1; k++) {
                close(pfds[k][0]);
                close(pfds[k][1]);
            }

            // 3) Apply redirections for this stage.
            //    NOTE: we apply redirs AFTER pipe dup2s so redirs override pipeline if both are present.
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

            // 4) Exec the stage
            execvp(S[i].argv[0], S[i].argv);
            perror(S[i].argv[0]);
            _exit(127);
        }
        // ---- parent continues ----
    }

    // Parent: close all pipe fds
    for (int k = 0; k < n-1; k++) { close(pfds[k][0]); close(pfds[k][1]); }
    free(pfds);

    // Wait for all children; the prompt timing is naturally gated by the last one.
    // (Spec minimum is to wait for the last process; reaping all avoids zombies.) :contentReference[oaicite:6]{index=6}
    int status;
    for (int i = 0; i < n; i++) {
        waitpid(pids[i], &status, 0);
    }
    free(pids);
    return 0;
}