// client.c
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <arpa/inet.h>
#include "net.h"

static int send_frame(int fd, const char *buf, uint32_t len) {
    uint32_t be = htonl(len);
    if (writen(fd, &be, 4) != 4) return -1;
    if (len && writen(fd, buf, len) != (ssize_t)len) return -1;
    return 0;
}

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

int main(int argc, char **argv) {
    if (argc != 3) { fprintf(stderr, "Usage: %s <server_host> <port>\n", argv[0]); return 1; }
    const char *host = argv[1];
    uint16_t port = (uint16_t)atoi(argv[2]);

    int fd = tcp_connect(host, port);
    if (fd < 0) { perror("connect"); return 1; }

    char *line = NULL; size_t cap = 0;
    for (;;) {
        printf("$ "); fflush(stdout);
        ssize_t r = getline(&line, &cap, stdin);
        if (r == -1) break;

        // send the line (including newline)
        if (send_frame(fd, line, (uint32_t)r) < 0) { fprintf(stderr, "send error\n"); break; }

        // read reply (may be empty)
        char *out = NULL; uint32_t olen = 0;
        if (recv_frame(fd, &out, &olen) < 0) { fprintf(stderr, "recv error\n"); break; }
        if (olen) {
            // print server’s command output exactly like a local shell would
            fwrite(out, 1, olen, stdout);
            free(out);
        } else {
            // zero-length frame → session closing (e.g., after 'exit')
            free(out);
            break;
        }
    }

    free(line);
    close(fd);
    return 0;
}
