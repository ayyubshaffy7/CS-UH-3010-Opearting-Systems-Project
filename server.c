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
                               const char **errmsg, char **err_buf, size_t *err_len)
{
    *out_buf = NULL; *out_len = 0; *errmsg = NULL;
    if (err_buf) *err_buf = NULL;
    if (err_len) *err_len = 0;

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

    // Pipes to capture child's stdout+stderr
    int out_pfd[2]; 
    int err_pfd[2]; 
    if (pipe(out_pfd) < 0) { free(stages); return -1; }
    if (pipe(err_pfd) < 0) { close(out_pfd[0]); close(out_pfd[1]); free(stages); return -1; }

    pid_t pid = fork();
    if (pid < 0) { 
        close(out_pfd[0]); close(out_pfd[1]);
        close(err_pfd[0]); close(err_pfd[1]);
        free(stages);
        return -1;
    }

    if (pid == 0) {
        close(out_pfd[0]); close(err_pfd[0]);

        if (dup2(out_pfd[1], STDOUT_FILENO) < 0) _exit(127);
        if (dup2(out_pfd[1], STDERR_FILENO) < 0) _exit(127);
        // keep out_pfd[1] open as stdout/stderr
        // pass err_pfd[1] to exec_pipeline for mirroring error lines:
        exec_pipeline(stages, nstages, err_pfd[1]);
        // close write end before exit so parent sees EOF
        close(err_pfd[1]);
        exit(0);
    }

    // Parent: read everything the child writes
    close(out_pfd[1]); close(err_pfd[1]);  // close write ends, read both streams

    // read OUT payload
    size_t ocap = 4096, olen = 0; char *obuf = malloc(ocap);
    if (!obuf) { close(out_pfd[0]); close(err_pfd[0]); free(stages); return -1; }
    for (;;) {
        char tmp[4096]; ssize_t r = read(out_pfd[0], tmp, sizeof tmp);
        if (r < 0) { if (errno == EINTR) continue; free(obuf); close(out_pfd[0]); close(err_pfd[0]); free(stages); return -1; }
        if (r == 0) break;
        if (olen + (size_t)r > ocap) { ocap = (olen + (size_t)r) * 2; char *nb = realloc(obuf, ocap); if (!nb){ free(obuf); close(out_pfd[0]); close(err_pfd[0]); free(stages); return -1;} obuf = nb; }
        memcpy(obuf + olen, tmp, (size_t)r); olen += (size_t)r;
    }
    close(out_pfd[0]);

    // read ERR payload (may be empty)
    size_t ecap = 256, elen = 0; char *ebuf = malloc(ecap);
    if (!ebuf) { free(obuf); close(err_pfd[0]); free(stages); return -1; }
    for (;;) {
        char tmp[256]; ssize_t r = read(err_pfd[0], tmp, sizeof tmp);
        if (r < 0) { if (errno == EINTR) continue; free(ebuf); free(obuf); close(err_pfd[0]); free(stages); return -1; }
        if (r == 0) break;
        if (elen + (size_t)r > ecap) { ecap = (elen + (size_t)r) * 2; char *nb = realloc(ebuf, ecap); if (!nb){ free(ebuf); free(obuf); close(err_pfd[0]); free(stages); return -1;} ebuf = nb; }
        memcpy(ebuf + elen, tmp, (size_t)r); elen += (size_t)r;
    }
    close(err_pfd[0]);

    int status; waitpid(pid, &status, 0);
    free(stages);

    *out_buf = obuf; *out_len = olen;
    if (err_buf) *err_buf = ebuf; else free(ebuf);
    if (err_len) *err_len = elen;
    return 0;
}

// Simple length-prefixed protocol: [uint32 length][payload]
static int recv_frame(int fd, char **buf, uint32_t *len) {
    uint32_t be;
    ssize_t r = readn(fd, &be, 4);
    if (r == 0) {                 // peer closed before sending header
        return 1;                 // <-- signal clean disconnect
    }
    if (r < 0) return -1;         // I/O error
    if (r != 4) return -2;        // partial header -> protocol error

    *len = ntohl(be);
    *buf = NULL;

    if (*len == 0) return 0;      // empty frame is valid (e.g., exit ACK)

    *buf = malloc(*len + 1);
    if (!*buf) return -1;

    r = readn(fd, *buf, *len);
    if (r < 0) { free(*buf); *buf = NULL; return -1; }
    if ((uint32_t)r != *len) { free(*buf); *buf = NULL; return -2; }

    (*buf)[*len] = '\0';
    return 0;
}

static int send_frame(int fd, const char *buf, uint32_t len) {
    uint32_t be = htonl(len);
    if (writen(fd, &be, 4) != 4) return -1;
    if (len && writen(fd, buf, len) != (ssize_t)len) return -1;
    return 0;
}

int main() {
    uint16_t port = 5050;
    int lfd = tcp_listen(port);
    if (lfd < 0) { perror("listen"); return 1; }
    LOG_INFO("Server started, waiting for client connections on port %u...", port);

    for (;;) {
        struct sockaddr_in peer; socklen_t pl = sizeof peer;
        int cfd = accept(lfd, (struct sockaddr*)&peer, &pl);
        if (cfd < 0) { if (errno == EINTR) continue; perror("accept"); break; }

        char ip[32]; inet_ntop(AF_INET, &peer.sin_addr, ip, sizeof ip);
        LOG_INFO("Client connected.");

        for (;;) {
            char *cmd = NULL; uint32_t clen = 0;
            int rf = recv_frame(cfd, &cmd, &clen);
            if (rf == 1) {                        // clean EOF (Ctrl+C or client quit)
                break;
            }
            if (rf < 0) {                         // -1 I/O error, -2 protocol error
                LOG_ERR("Receive error from client.");
                break;
            }
            if (clen == 0) {                      // valid empty frame (e.g., after 'exit')
                free(cmd);
                break;
            }

            // Trim trailing newline(s)
            while (clen && (cmd[clen-1] == '\n' || cmd[clen-1] == '\r')) cmd[--clen] = '\0';

            LOG_RX("Received command: \"%s\" from client.", cmd);

            if (strcmp(cmd, "exit") == 0) {
            LOG_INFO("Closing session on 'exit'.");
            free(cmd);
            // special 0xFFFFFFFF control frame for clean disconnect
            uint32_t close_flag = htonl(0xFFFFFFFF);
            writen(cfd, &close_flag, 4);

            break;
            }

            // Tokenize (Phase-1)
            char **tokens = parse_command(cmd);

            // Execute with capture
            LOG_EX("Executing command: \"%s\"", cmd);
            char *payload = NULL; size_t plen = 0;
            char *errtxt = NULL; size_t elen = 0;
            const char *perr = NULL;
            if (run_command_capture(tokens, &payload, &plen, &perr, &errtxt, &elen) < 0) {
                LOG_ERR("Internal failure (pipe/fork).");
                const char *msg = "internal error\n";
                send_frame(cfd, msg, (uint32_t)strlen(msg));
            } 
            else {
                if (perr) {
                    LOG_ERR("%s", perr);
                    LOG_OUT("Sending error message to client: \"%s\"", perr);
                } else if (elen > 0) {
                    // We got explicit error text from exec failure
                    // The first token is in `cmd` already; log like the rubric:
                    LOG_ERR("Command not found: \"%s\"", cmd);
                    size_t eshow = elen > 2000 ? 2000 : elen;
                    if (eshow == 0) {
                        LOG_OUT("Sending error message to client: (empty)");
                    } else {
                        LOG_OUT("Sending error message to client: \"%.*s%s\"",
                                (int)eshow, errtxt,
                                (elen > eshow ? "\n..." : ""));
                    }
                } else {
                    size_t show = plen > 2000 ? 2000 : plen;   // cap to keep logs sane
                    if (show == 0) {
                        LOG_OUT("Sending output to client: (empty)");
                    } else {
                        LOG_OUT("Sending output to client:\n%.*s%s",
                                (int)show, payload,
                                (plen > show ? "\n..." : ""));
                    }
                }
                send_frame(cfd, payload, (uint32_t)plen);
            }
            free(errtxt);
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
