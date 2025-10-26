#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdint.h>
#include <stdarg.h> 
#include <sys/wait.h>
#include <fcntl.h>
#include <errno.h>
#include <arpa/inet.h>
#include "net.h"
#include "utils.h"

static void log_line(const char *tag, const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    fprintf(stderr, "[%s] ", tag);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
}

#define LOG_INFO(...) log_line("INFO", __VA_ARGS__)
#define LOG_RX(...)   log_line("RECEIVED", __VA_ARGS__)
#define LOG_EX(...)   log_line("EXECUTING", __VA_ARGS__)
#define LOG_OUT(...)  log_line("OUTPUT", __VA_ARGS__)
#define LOG_ERR(...)  log_line("ERROR", __VA_ARGS__)

// show a printable preview of payload (newlines escaped) up to N chars
static void preview_payload(const char *data, size_t len, size_t N, char *out, size_t outcap) {
    size_t w = 0;
    for (size_t i = 0; i < len && w + 4 < outcap && N > 0; i++, N--) {
        unsigned char c = (unsigned char)data[i];
        if (c == '\n') { out[w++]='\\'; out[w++]='n'; }
        else if (c == '\r') { out[w++]='\\'; out[w++]='r'; }
        else if (c == '\t') { out[w++]='\\'; out[w++]='t'; }
        else if (c < 32 || c == 127) { w += snprintf(out+w, outcap-w, "\\x%02X", c); }
        else out[w++] = (char)c;
    }
    out[w] = '\0';
}

// server.c
// Capture combined stdout+stderr of a parsed command line.
// On parse error (bad pipes/redirs), we return that error as text payload (newline-terminated)
// so the client prints it like a real shell would.
//
// Returns 0 on success (payload captured and returned).
// Returns -1 only on immediate setup failure (pipe/fork/malloc).
//
// Ownership:
//   - *out_buf is malloc'd here; caller must free(*out_buf).
//   - *errmsg is only used for internal branching; we materialize parse errors into out_buf.
static int run_command_capture(char **tokens,
                               char **out_buf, size_t *out_len,
                               const char **errmsg)
{
    *out_buf = NULL; *out_len = 0; *errmsg = NULL;

    // Build pipeline
    Stage *stages = NULL;
    int nstages = 0;
    if (build_pipeline(tokens, &stages, &nstages, errmsg) < 0) {
        // Convert parse-time error into textual payload for the client.
        size_t l = strlen(*errmsg);
        char *buf = malloc(l + 2);
        if (!buf) return -1;
        memcpy(buf, *errmsg, l);
        buf[l] = '\n';
        buf[l+1] = '\0';
        *out_buf = buf;
        *out_len = l + 1;
        *errmsg = NULL; // handled as payload
        return 0;
    }

    // Pipe to capture child's stdout+stderr
    int pfd[2];
    if (pipe(pfd) < 0) { free(stages); return -1; }

    pid_t pid = fork();
    if (pid < 0) { close(pfd[0]); close(pfd[1]); free(stages); return -1; }

    if (pid == 0) {
        // Child: route both stdout and stderr into pfd[1], then run pipeline.
        close(pfd[0]);
        if (dup2(pfd[1], STDOUT_FILENO) < 0) exit(127);
        if (dup2(pfd[1], STDERR_FILENO) < 0) exit(127);
        close(pfd[1]);
        exec_pipeline(stages, nstages);
        exit(0);
    }

    // Parent: read everything the child writes
    close(pfd[1]);
    size_t cap = 4096, len = 0;
    char *buf = malloc(cap);
    if (!buf) { close(pfd[0]); free(stages); return -1; }

    for (;;) {
        char tmp[4096];
        ssize_t r = read(pfd[0], tmp, sizeof tmp);
        if (r < 0) {
            if (errno == EINTR) continue;
            free(buf); close(pfd[0]); free(stages); return -1;
        }
        if (r == 0) break; // EOF
        if (len + (size_t)r > cap) {
            cap = (len + (size_t)r) * 2;
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

    *out_buf = buf;
    *out_len = len;
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
    LOG_INFO("Server started, waiting for client connections on port %u...", port);

    for (;;) {
        struct sockaddr_in peer; socklen_t pl = sizeof peer;
        int cfd = accept(lfd, (struct sockaddr*)&peer, &pl);
        if (cfd < 0) { if (errno == EINTR) continue; perror("accept"); break; }

        char ip[32]; inet_ntop(AF_INET, &peer.sin_addr, ip, sizeof ip);
        LOG_INFO("Client connected from %s:%u.", ip, ntohs(peer.sin_port));

        for (;;) {
            char *cmd = NULL; uint32_t clen = 0;
            if (recv_frame(cfd, &cmd, &clen) < 0) { LOG_ERR("Receive error or closed by peer."); break; }
            if (clen == 0) { free(cmd); break; }

            // Trim trailing newline(s)
            while (clen && (cmd[clen-1] == '\n' || cmd[clen-1] == '\r')) cmd[--clen] = '\0';

            LOG_RX("Received command: \"%s\" from client.", cmd);

            if (strcmp(cmd, "exit") == 0) {
                LOG_INFO("Closing session on 'exit'.");
                free(cmd); send_frame(cfd, "", 0); break;
            }

            // Tokenize (Phase-1)
            char **tokens = parse_command(cmd);

            // Execute with capture
            LOG_EX("Executing command: \"%s\"", cmd);
            char *payload = NULL; size_t plen = 0; const char *errmsg = NULL;
            if (run_command_capture(tokens, &payload, &plen, &errmsg) < 0) {
                LOG_ERR("Internal failure (pipe/fork).");
                const char *msg = "internal error\n";
                send_frame(cfd, msg, (uint32_t)strlen(msg));
            } 
            else {
                if (errmsg) {
                    // parse-time error (syntax/redirs/pipes)
                    LOG_ERR("%s", errmsg);
                    LOG_OUT("Sending error message to client: \"%s\"", errmsg);
                }
                else {
                    char prev[160]; preview_payload(payload, plen, 120, prev, sizeof prev);
                    LOG_OUT("Sending output to client: %s", prev[0] ? prev : "(empty)");
                }
            }
            send_frame(cfd, payload, (uint32_t)plen);
            free(payload);
            free(tokens);
            free(cmd);
        }

        close(cfd);
        LOG_INFO("Client disconnected.");
    }

    close(lfd);
    return 0;
}
