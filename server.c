// server.c
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdint.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <errno.h>
#include <arpa/inet.h>
#include "net.h"
#include "utils.h"      // your parse_command (quote-aware), etc.

// Capture exec_pipeline's combined stdout/stderr into a dynamic buffer.
// Strategy: fork a child that runs exec_pipeline with stdout/stderr duped to pipe[1].
// Parent reads all bytes from pipe[0] into malloc'd buffer.
static int run_command_capture(char **tokens, char **out_buf, size_t *out_len, const char **errmsg) {
    *out_buf = NULL; *out_len = 0; *errmsg = NULL;

    Stage *stages = NULL;
    int nstages = 0;
    if (build_pipeline(tokens, &stages, &nstages, errmsg) < 0) {
        // return the parse error as payload back to client
        size_t l = strlen(*errmsg);
        *out_buf = malloc(l + 2);
        memcpy(*out_buf, *errmsg, l);
        (*out_buf)[l] = '\n'; (*out_buf)[l+1] = '\0';
        *out_len = l + 1;
        *errmsg = NULL; // handled as textual error
        return 0;
    }

    int pfd[2];
    if (pipe(pfd) < 0) { *errmsg = "pipe failed"; free(stages); return -1; }

    pid_t pid = fork();
    if (pid < 0) { *errmsg = "fork failed"; close(pfd[0]); close(pfd[1]); free(stages); return -1; }

    if (pid == 0) {
        // child: route stdout+stderr to pfd[1], then run pipeline
        close(pfd[0]);
        if (dup2(pfd[1], STDOUT_FILENO) < 0) _exit(127);
        if (dup2(pfd[1], STDERR_FILENO) < 0) _exit(127);
        // Close inherited pipe ends not needed:
        // (exec_pipeline will fork further and children inherit these std fds)
        // run
        exec_pipeline(stages, nstages);
        _exit(0);
    }

    // parent: read all
    close(pfd[1]);
    size_t cap = 4096, len = 0;
    char *buf = malloc(cap);
    if (!buf) { close(pfd[0]); free(stages); return -1; }

    for (;;) {
        char tmp[4096];
        ssize_t r = read(pfd[0], tmp, sizeof tmp);
        if (r < 0) { if (errno == EINTR) continue; break; }
        if (r == 0) break;
        if (len + (size_t)r > cap) {
            cap = (len + r) * 2;
            char *nb = realloc(buf, cap);
            if (!nb) { free(buf); close(pfd[0]); free(stages); return -1; }
            buf = nb;
        }
        memcpy(buf + len, tmp, (size_t)r);
        len += (size_t)r;
    }
    close(pfd[0]);

    int status; waitpid(pid, &status, 0);
    free(stages);
    *out_buf = buf; *out_len = len;
    return 0;
}

// Simple length-prefixed protocol: [uint32 length][payload]
static int recv_frame(int fd, char **buf, uint32_t *len) {
    uint32_t be;
    if (readn(fd, &be, 4) != 4) return -1;
    *len = ntohl(be);
    *buf = NULL;
    if (*len == 0) return 0;
    *buf = malloc(*len + 1);
    if (!*buf) return -1;
    if (readn(fd, *buf, *len) != (ssize_t)*len) { free(*buf); *buf = NULL; return -1; }
    (*buf)[*len] = '\0';
    return 0;
}

static int send_frame(int fd, const char *buf, uint32_t len) {
    uint32_t be = htonl(len);
    if (writen(fd, &be, 4) != 4) return -1;
    if (len && writen(fd, buf, len) != (ssize_t)len) return -1;
    return 0;
}

int main(int argc, char **argv) {
    if (argc != 2) { fprintf(stderr, "Usage: %s <port>\n", argv[0]); return 1; }
    uint16_t port = (uint16_t)atoi(argv[1]);
    int lfd = tcp_listen(port);
    if (lfd < 0) { perror("listen"); return 1; }
    fprintf(stderr, "[server] listening on %u\n", port); // server-side trace per spec. :contentReference[oaicite:1]{index=1}

    for (;;) {
        int cfd = accept(lfd, NULL, NULL);
        if (cfd < 0) { if (errno == EINTR) continue; perror("accept"); break; }
        fprintf(stderr, "[server] client connected\n");   // server trace

        for (;;) {
            char *cmd = NULL; uint32_t clen = 0;
            if (recv_frame(cfd, &cmd, &clen) < 0) { fprintf(stderr, "[server] recv error/closed\n"); break; }
            if (clen == 0) { free(cmd); break; } // client closed session
            // Trim trailing newlines (client sends line)
            while (clen && (cmd[clen-1] == '\n' || cmd[clen-1] == '\r')) cmd[--clen] = '\0';

            // Builtin exit ends session
            // NOTE: we do quote-aware parse, so catch trivial "exit" quickly:
            if (strcmp(cmd, "exit") == 0) {
                free(cmd);
                send_frame(cfd, "", 0); // ack/close
                break;
            }

            // Tokenize like Phase 1 (quote-aware); then run and capture.
            char **tokens = parse_command(cmd);
            char *payload = NULL; size_t plen = 0; const char *errmsg = NULL;
            if (run_command_capture(tokens, &payload, &plen, &errmsg) < 0) {
                const char *msg = errmsg ? errmsg : "internal error";
                send_frame(cfd, msg, (uint32_t)strlen(msg));
            } else {
                send_frame(cfd, payload, (uint32_t)plen);
            }

            // cleanup
            free(payload);
            free(tokens); // consistent with your Phase-1 ownership
            free(cmd);
        }

        close(cfd);
        fprintf(stderr, "[server] client disconnected\n");
    }

    close(lfd);
    return 0;
}
