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
#include <pthread.h>
#include "net.h"
#include "utils.h"

// This struct will be passed to each new thread.
// It contains all the info the thread needs for the session.
typedef struct {
    int cfd;                 // Client's file descriptor
    uint32_t client_id;      // Client's unique ID (e.g., 1, 2, 3...)
    struct sockaddr_in peer; // Client's address info
} client_info_t;

// A global counter for assigning unique client IDs
static int g_client_counter = 0;

// Standard log function (for non-prefixed messages)
static void log_line(const char *tag, const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    fprintf(stderr, "[%s] ", tag);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
}

// log function for thread-specific, prefixed messages
// e.g., [TAG] [Client #1 - 127.0.0.1:12345] ...
static void log_line_prefixed(const char *tag, const char *prefix, const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    fprintf(stderr, "[%s] %s ", tag, prefix); // [TAG] [Client #1 - ...]
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
}

#define LOG_INFO(...) log_line("INFO", __VA_ARGS__)
// We will call log_line_prefixed directly in the thread, so no other macros are needed.


// This function is UNCHANGED from Phase 2.
// It captures the output of the shell logic from utils.c
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
        size_t l = strlen(*errmsg);
        char *buf = malloc(l + 2);
        if (!buf) return -1;
        memcpy(buf, *errmsg, l);
        buf[l] = '\n';
        buf[l+1] = '\0';
        *out_buf = buf;
        *out_len = l + 1;
        *errmsg = NULL;
        return 0;
    }

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
        exec_pipeline(stages, nstages, err_pfd[1]);
        close(err_pfd[1]);
        exit(0);
    }

    close(out_pfd[1]); close(err_pfd[1]);

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

// These two framing functions are UNCHANGED from Phase 2.
static int recv_frame(int fd, char **buf, uint32_t *len) {
    uint32_t be;
    ssize_t r = readn(fd, &be, 4);
    if (r == 0) { return 1; }
    if (r < 0) return -1;
    if (r != 4) return -2;

    *len = ntohl(be);
    *buf = NULL;

    if (*len == 0) return 0;

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

// ALL of the client session logic from Phase 2's main() is moved to this thread function
void *client_thread_func(void *arg) {
    // Detach the thread so its resources are automatically freed on exit
    pthread_detach(pthread_self());

    // Unpack the client info
    client_info_t *info = (client_info_t *)arg;
    int cfd = info->cfd;
    int client_id = info->client_id;

    // Create the log prefix string for this client
    char ip[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &info->peer.sin_addr, ip, sizeof ip);
    uint16_t port = ntohs(info->peer.sin_port);
    char client_prefix[128];
    snprintf(client_prefix, sizeof client_prefix, "[Client #%d - %s:%u]", client_id, ip, port);

    // Log the "connected" message (matches spec)
    LOG_INFO("Client #%d connected from %s:%u. Assigned to Thread-%d.",
             client_id, ip, port, client_id);

    // This is the client session loop, (copied from Phase 2's main)
    for (;;) {
        char *cmd = NULL; uint32_t clen = 0;
        int rf = recv_frame(cfd, &cmd, &clen);
        if (rf == 1) { break; } // clean EOF
        if (rf < 0) {
            log_line_prefixed("ERROR", client_prefix, "Receive error from client.");
            break;
        }
        if (clen == 0) {
            free(cmd);
            break;
        }

        while (clen && (cmd[clen-1] == '\n' || cmd[clen-1] == '\r')) cmd[--clen] = '\0';

        // Use the new log format
        fprintf(stderr, "\n");
        log_line_prefixed("RECEIVED", client_prefix, "Received command: \"%s\"", cmd);

        if (strcmp(cmd, "exit") == 0) {
            log_line_prefixed("INFO", client_prefix, "Client requested disconnect. Closing connection.");
            free(cmd);
            uint32_t close_flag = htonl(0xFFFFFFFF);
            writen(cfd, &close_flag, 4);
            break;
        }

        char **tokens = parse_command(cmd);
        log_line_prefixed("EXECUTING", client_prefix, "Executing command: \"%s\"", cmd);

        char *payload = NULL; size_t plen = 0;
        char *errtxt = NULL; size_t elen = 0;
        const char *perr = NULL;
        if (run_command_capture(tokens, &payload, &plen, &perr, &errtxt, &elen) < 0) {
            log_line_prefixed("ERROR", client_prefix, "Internal failure (pipe/fork).");
            const char *msg = "internal error\n";
            send_frame(cfd, msg, (uint32_t)strlen(msg));
        }
        else {
            if (perr) {
                log_line_prefixed("ERROR", client_prefix, "%s", perr);
                log_line_prefixed("OUTPUT", client_prefix, "Sending error message to client: \"%s\"", perr);
            } else if (elen > 0) {
                while (elen && (errtxt[elen-1] == '\n' || errtxt[elen-1] == '\r')) errtxt[--elen] = '\0';
                log_line_prefixed("ERROR", client_prefix, "Command not found: \"%s\"", cmd);
                log_line_prefixed("OUTPUT", client_prefix, "Sending error message to client: \"%.*s\"", (int)elen, errtxt);
            } else {
                size_t show = plen > 2000 ? 2000 : plen;
                if (show == 0) {
                    log_line_prefixed("OUTPUT", client_prefix,
                                    "Sending output to client: (empty)");
                } else {
                    // Trim trailing newlines from the preview so we don't end up with
                    // extra blank lines in the log. (Only for logging; payload sent is unchanged.)
                    size_t trimmed = show;
                    while (trimmed > 0 &&
                        (payload[trimmed-1] == '\n' || payload[trimmed-1] == '\r')) {
                        trimmed--;
                    }
                    log_line_prefixed("OUTPUT", client_prefix, "Sending output to client:\n%.*s", (int)trimmed, payload);
                }
            }
            send_frame(cfd, payload, (uint32_t)plen);
        }
        free(errtxt);
        free(payload);
        free(tokens);
        free(cmd);
    }

    // Cleanup
    close(cfd);
    LOG_INFO("Client #%d disconnected.", client_id);
    free(info);
    return NULL;
}

// Main is now MUCH simpler.
// It just accepts connections and spawns threads.
int main() {
    uint16_t port = 5050;
    int lfd = tcp_listen(port);
    if (lfd < 0) { perror("listen"); return 1; }
    LOG_INFO("Server started, waiting for client connections on port %u...", port);

    for (;;) {
        struct sockaddr_in peer; socklen_t pl = sizeof peer;
        int cfd = accept(lfd, (struct sockaddr*)&peer, &pl);
        if (cfd < 0) { if (errno == EINTR) continue; perror("accept"); break; }

        // Malloc a new info struct for this client
        client_info_t *info = malloc(sizeof(client_info_t));
        if (!info) {
            LOG_INFO("Failed to allocate memory for new client.");
            close(cfd);
            continue;
        }

        // Fill the struct
        info->cfd = cfd;
        info->client_id = ++g_client_counter;
        info->peer = peer;

        // Create the new thread
        pthread_t tid;
        if (pthread_create(&tid, NULL, client_thread_func, info) != 0) {
            LOG_INFO("Failed to create thread for client #%d", info->client_id);
            free(info);
            close(cfd);
        }
    }

    close(lfd);
    return 0;
}
