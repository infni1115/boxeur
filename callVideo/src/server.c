/*
 * server.c - lightweight WebRTC signaling + static file server, in plain C.
 *
 * It does exactly two jobs:
 *   1. Serve the static frontend (index.html / app.js / style.css).
 *   2. Speak just enough WebSocket (RFC 6455) to relay JSON signaling
 *      messages (SDP offers/answers, ICE candidates) between two browsers
 *      that joined the same "room". Actual audio/video never touches this
 *      server - it flows peer-to-peer between the two browsers via WebRTC.
 *
 * No external dependencies beyond POSIX sockets + pthreads. SHA1 and
 * base64, needed only for the WebSocket handshake, are implemented locally
 * (see sha1.c / base64.c).
 *
 * Build:  make
 * Run:    ./bin/server [port] [docroot]      (defaults: 8080, ./public)
 */

#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <ctype.h>
#include <errno.h>
#include <signal.h>
#include <stdint.h>

#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <sys/stat.h>
#include <pthread.h>

#include "sha1.h"
#include "base64.h"

#define DEFAULT_PORT 8080
#define DEFAULT_DOCROOT "public"
#define WS_GUID "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"
#define MAX_HEADER_SIZE 16384
#define MAX_WS_MESSAGE (256 * 1024)
#define MAX_ROOMS 64
#define ROOM_NAME_MAX 64

static const char *g_docroot = DEFAULT_DOCROOT;

/* ---------------------------------------------------------------------- */
/* Room table: each room holds at most two peers (this app is strictly    */
/* one-on-one). Guarded by a single mutex - traffic is tiny so this never */
/* becomes a bottleneck.                                                  */
/* ---------------------------------------------------------------------- */

typedef struct {
    int used;
    char name[ROOM_NAME_MAX];
    int fd[2];
} room_t;

static room_t rooms[MAX_ROOMS];
static pthread_mutex_t rooms_lock = PTHREAD_MUTEX_INITIALIZER;

/* Add `fd` to room `name`. Returns:
 *   0  -> joined alone (room created), *peer_fd = -1
 *   1  -> joined as second peer, *peer_fd = the other socket
 *  -1  -> room already full, rejected
 */
static int room_join(const char *name, int fd, int *peer_fd) {
    pthread_mutex_lock(&rooms_lock);

    room_t *found = NULL;
    room_t *free_slot = NULL;
    for (int i = 0; i < MAX_ROOMS; i++) {
        if (rooms[i].used && strncmp(rooms[i].name, name, ROOM_NAME_MAX) == 0) {
            found = &rooms[i];
            break;
        }
        if (!rooms[i].used && !free_slot) free_slot = &rooms[i];
    }

    int result;
    if (found) {
        if (found->fd[0] != -1 && found->fd[1] != -1) {
            result = -1; /* room full */
            *peer_fd = -1;
        } else if (found->fd[0] == -1) {
            found->fd[0] = fd;
            *peer_fd = found->fd[1];
            result = (found->fd[1] != -1) ? 1 : 0;
        } else {
            found->fd[1] = fd;
            *peer_fd = found->fd[0];
            result = 1;
        }
    } else if (free_slot) {
        free_slot->used = 1;
        strncpy(free_slot->name, name, ROOM_NAME_MAX - 1);
        free_slot->name[ROOM_NAME_MAX - 1] = '\0';
        free_slot->fd[0] = fd;
        free_slot->fd[1] = -1;
        *peer_fd = -1;
        result = 0;
    } else {
        result = -1; /* no room left at all */
        *peer_fd = -1;
    }

    pthread_mutex_unlock(&rooms_lock);
    return result;
}

/* Remove `fd` from whatever room it's in. Returns the peer fd (if any, so
 * the caller can notify it that we left), or -1. */
static int room_leave(int fd) {
    int peer = -1;
    pthread_mutex_lock(&rooms_lock);
    for (int i = 0; i < MAX_ROOMS; i++) {
        if (!rooms[i].used) continue;
        if (rooms[i].fd[0] == fd) {
            peer = rooms[i].fd[1];
            rooms[i].fd[0] = -1;
        } else if (rooms[i].fd[1] == fd) {
            peer = rooms[i].fd[0];
            rooms[i].fd[1] = -1;
        } else {
            continue;
        }
        if (rooms[i].fd[0] == -1 && rooms[i].fd[1] == -1) rooms[i].used = 0;
        break;
    }
    pthread_mutex_unlock(&rooms_lock);
    return peer;
}

/* Current peer fd for `fd` (re-looked-up on every relayed message, in case
 * of reconnects). -1 if alone. */
static int room_peer_of(int fd) {
    int peer = -1;
    pthread_mutex_lock(&rooms_lock);
    for (int i = 0; i < MAX_ROOMS; i++) {
        if (!rooms[i].used) continue;
        if (rooms[i].fd[0] == fd) { peer = rooms[i].fd[1]; break; }
        if (rooms[i].fd[1] == fd) { peer = rooms[i].fd[0]; break; }
    }
    pthread_mutex_unlock(&rooms_lock);
    return peer;
}

/* ---------------------------------------------------------------------- */
/* Small socket helpers                                                   */
/* ---------------------------------------------------------------------- */

static ssize_t send_all(int fd, const void *buf, size_t len) {
    const char *p = buf;
    size_t sent = 0;
    while (sent < len) {
        ssize_t n = send(fd, p + sent, len - sent, 0);
        if (n <= 0) {
            if (n < 0 && errno == EINTR) continue;
            return -1;
        }
        sent += (size_t)n;
    }
    return (ssize_t)sent;
}

static int recv_all(int fd, void *buf, size_t len) {
    char *p = buf;
    size_t got = 0;
    while (got < len) {
        ssize_t n = recv(fd, p + got, len - got, 0);
        if (n == 0) return -1;          /* peer closed */
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        got += (size_t)n;
    }
    return 0;
}

/* ---------------------------------------------------------------------- */
/* WebSocket framing (RFC 6455)                                           */
/* ---------------------------------------------------------------------- */

#define WS_OP_CONT  0x0
#define WS_OP_TEXT  0x1
#define WS_OP_BIN   0x2
#define WS_OP_CLOSE 0x8
#define WS_OP_PING  0x9
#define WS_OP_PONG  0xA

/* Sends a single unmasked frame (server -> client never masks). */
static int ws_send_frame(int fd, int opcode, const unsigned char *payload, size_t len) {
    unsigned char header[10];
    size_t hlen = 0;

    header[hlen++] = (unsigned char)(0x80 | (opcode & 0x0F)); /* FIN + opcode */

    if (len <= 125) {
        header[hlen++] = (unsigned char)len;
    } else if (len <= 0xFFFF) {
        header[hlen++] = 126;
        header[hlen++] = (unsigned char)((len >> 8) & 0xFF);
        header[hlen++] = (unsigned char)(len & 0xFF);
    } else {
        header[hlen++] = 127;
        for (int i = 7; i >= 0; i--)
            header[hlen++] = (unsigned char)((len >> (8 * i)) & 0xFF);
    }

    if (send_all(fd, header, hlen) < 0) return -1;
    if (len > 0 && send_all(fd, payload, len) < 0) return -1;
    return 0;
}

static int ws_send_text(int fd, const char *msg) {
    return ws_send_frame(fd, WS_OP_TEXT, (const unsigned char *)msg, strlen(msg));
}

/* Reads exactly one frame (header + payload), unmasking client payloads.
 * Returns 0 on success (fills *opcode, *fin, *payload [malloc'd, caller
 * frees], *paylen), -1 on error/close. */
static int ws_read_frame(int fd, int *opcode, int *fin, unsigned char **payload, size_t *paylen) {
    unsigned char hdr[2];
    if (recv_all(fd, hdr, 2) < 0) return -1;

    *fin = (hdr[0] & 0x80) != 0;
    *opcode = hdr[0] & 0x0F;
    int masked = (hdr[1] & 0x80) != 0;
    uint64_t len = hdr[1] & 0x7F;

    if (len == 126) {
        unsigned char ext[2];
        if (recv_all(fd, ext, 2) < 0) return -1;
        len = ((uint64_t)ext[0] << 8) | ext[1];
    } else if (len == 127) {
        unsigned char ext[8];
        if (recv_all(fd, ext, 8) < 0) return -1;
        len = 0;
        for (int i = 0; i < 8; i++) len = (len << 8) | ext[i];
    }

    if (len > MAX_WS_MESSAGE) return -1;

    unsigned char mask[4] = {0, 0, 0, 0};
    if (masked) {
        if (recv_all(fd, mask, 4) < 0) return -1;
    }

    unsigned char *buf = NULL;
    if (len > 0) {
        buf = malloc(len);
        if (!buf) return -1;
        if (recv_all(fd, buf, len) < 0) { free(buf); return -1; }
        if (masked) {
            for (uint64_t i = 0; i < len; i++) buf[i] ^= mask[i % 4];
        }
    }

    *payload = buf;
    *paylen = (size_t)len;
    return 0;
}

/* Reads one full (possibly fragmented) text message. Returns malloc'd,
 * NUL-terminated buffer via *out, or NULL on error/close. Handles ping and
 * silently drops anything that isn't text. */
static char *ws_read_message(int fd) {
    unsigned char *acc = NULL;
    size_t acc_len = 0;
    int have_start = 0;

    for (;;) {
        int opcode, fin;
        unsigned char *payload = NULL;
        size_t paylen = 0;

        if (ws_read_frame(fd, &opcode, &fin, &payload, &paylen) < 0) {
            free(acc);
            return NULL;
        }

        if (opcode == WS_OP_CLOSE) {
            free(payload);
            free(acc);
            return NULL;
        }
        if (opcode == WS_OP_PING) {
            ws_send_frame(fd, WS_OP_PONG, payload, paylen);
            free(payload);
            continue;
        }
        if (opcode == WS_OP_PONG) {
            free(payload);
            continue;
        }

        if (opcode == WS_OP_TEXT || opcode == WS_OP_BIN) {
            have_start = 1;
            free(acc);
            acc = payload;
            acc_len = paylen;
        } else if (opcode == WS_OP_CONT && have_start) {
            unsigned char *bigger = realloc(acc, acc_len + paylen);
            if (!bigger) { free(acc); free(payload); return NULL; }
            memcpy(bigger + acc_len, payload, paylen);
            acc = bigger;
            acc_len += paylen;
            free(payload);
        } else {
            free(payload); /* unknown/unsupported opcode, ignore */
            continue;
        }

        if (fin) {
            char *out = malloc(acc_len + 1);
            if (!out) { free(acc); return NULL; }
            if (acc_len) memcpy(out, acc, acc_len);
            out[acc_len] = '\0';
            free(acc);
            return out;
        }
    }
}

/* ---------------------------------------------------------------------- */
/* Minimal HTTP: request line/header parsing, static file serving, and    */
/* the WebSocket upgrade handshake.                                       */
/* ---------------------------------------------------------------------- */

/* Reads request headers into buf (NUL-terminated) up to the blank line.
 * Returns total bytes read (excluding NUL), or -1 on error/overflow. */
static ssize_t read_http_headers(int fd, char *buf, size_t cap) {
    size_t total = 0;
    while (total + 1 < cap) {
        ssize_t n = recv(fd, buf + total, 1, 0);
        if (n <= 0) {
            if (n < 0 && errno == EINTR) continue;
            return -1;
        }
        total += (size_t)n;
        buf[total] = '\0';
        if (total >= 4 && memcmp(buf + total - 4, "\r\n\r\n", 4) == 0) return (ssize_t)total;
    }
    return -1;
}

/* Case-insensitive search for a header's value within the raw header
 * block. Returns a malloc'd, trimmed copy, or NULL if absent. */
static char *header_value(const char *headers, const char *name) {
    size_t namelen = strlen(name);
    const char *p = headers;
    while (*p) {
        const char *line_end = strstr(p, "\r\n");
        if (!line_end) break;
        if ((size_t)(line_end - p) > namelen && strncasecmp(p, name, namelen) == 0 && p[namelen] == ':') {
            const char *v = p + namelen + 1;
            while (*v == ' ' || *v == '\t') v++;
            size_t vlen = (size_t)(line_end - v);
            char *out = malloc(vlen + 1);
            if (out) { memcpy(out, v, vlen); out[vlen] = '\0'; }
            return out;
        }
        p = line_end + 2;
        if (p[0] == '\0' || (p[0] == '\r' && p[1] == '\n')) break;
    }
    return NULL;
}

static int header_contains_ci(const char *headers, const char *name, const char *needle) {
    char *v = header_value(headers, name);
    if (!v) return 0;
    int found = 0;
    size_t nlen = strlen(needle);
    for (char *s = v; *s; s++) {
        if (strncasecmp(s, needle, nlen) == 0) { found = 1; break; }
    }
    free(v);
    return found;
}

static const char *content_type_for(const char *path) {
    const char *dot = strrchr(path, '.');
    if (!dot) return "application/octet-stream";
    if (strcmp(dot, ".html") == 0) return "text/html; charset=utf-8";
    if (strcmp(dot, ".js") == 0) return "text/javascript; charset=utf-8";
    if (strcmp(dot, ".css") == 0) return "text/css; charset=utf-8";
    if (strcmp(dot, ".json") == 0) return "application/json";
    if (strcmp(dot, ".ico") == 0) return "image/x-icon";
    if (strcmp(dot, ".png") == 0) return "image/png";
    if (strcmp(dot, ".svg") == 0) return "image/svg+xml";
    return "application/octet-stream";
}

static void send_status(int fd, int code, const char *text) {
    char buf[256];
    int n = snprintf(buf, sizeof(buf),
        "HTTP/1.1 %d %s\r\nContent-Length: 0\r\nConnection: close\r\n\r\n", code, text);
    send_all(fd, buf, (size_t)n);
}

static void serve_static(int fd, const char *raw_path) {
    /* Strip query string, reject path traversal. */
    char path[1024];
    size_t i = 0;
    for (; raw_path[i] && raw_path[i] != '?' && i < sizeof(path) - 1; i++) path[i] = raw_path[i];
    path[i] = '\0';

    if (strstr(path, "..") != NULL) { send_status(fd, 403, "Forbidden"); return; }
    if (strcmp(path, "/") == 0) strcpy(path, "/index.html");

    char fullpath[2048];
    snprintf(fullpath, sizeof(fullpath), "%s%s", g_docroot, path);

    struct stat st;
    if (stat(fullpath, &st) != 0 || !S_ISREG(st.st_mode)) {
        send_status(fd, 404, "Not Found");
        return;
    }

    FILE *f = fopen(fullpath, "rb");
    if (!f) { send_status(fd, 404, "Not Found"); return; }

    char header[512];
    int hlen = snprintf(header, sizeof(header),
        "HTTP/1.1 200 OK\r\nContent-Type: %s\r\nContent-Length: %lld\r\nConnection: close\r\n\r\n",
        content_type_for(path), (long long)st.st_size);
    send_all(fd, header, (size_t)hlen);

    char buf[8192];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
        if (send_all(fd, buf, n) < 0) break;
    }
    fclose(f);
}

/* Extracts the value of query parameter `key` from a path like
 * "/ws?room=abc". Returns a malloc'd copy, or NULL if absent/empty. */
static char *query_param(const char *path, const char *key) {
    const char *q = strchr(path, '?');
    if (!q) return NULL;
    q++;
    size_t klen = strlen(key);
    while (*q) {
        const char *amp = strchr(q, '&');
        size_t seglen = amp ? (size_t)(amp - q) : strlen(q);
        if (seglen > klen && strncmp(q, key, klen) == 0 && q[klen] == '=') {
            size_t vlen = seglen - klen - 1;
            if (vlen == 0 || vlen >= ROOM_NAME_MAX) return NULL;
            char *out = malloc(vlen + 1);
            if (out) { memcpy(out, q + klen + 1, vlen); out[vlen] = '\0'; }
            return out;
        }
        if (!amp) break;
        q = amp + 1;
    }
    return NULL;
}

/* ---------------------------------------------------------------------- */
/* Per-connection thread                                                  */
/* ---------------------------------------------------------------------- */

static void handle_websocket(int fd, const char *room) {
    int peer = -1;
    int joined_as = room_join(room, fd, &peer);
    if (joined_as < 0) {
        ws_send_text(fd, "{\"type\":\"error\",\"message\":\"room full\"}");
        return;
    }
    if (joined_as == 1 && peer != -1) {
        /* We are the second peer: tell the FIRST peer to become the
         * WebRTC offer initiator. We stay silent and wait for the offer. */
        ws_send_text(peer, "{\"type\":\"peer-joined\"}");
    }

    for (;;) {
        char *msg = ws_read_message(fd);
        if (!msg) break;

        int current_peer = room_peer_of(fd);
        if (current_peer != -1) {
            if (ws_send_text(current_peer, msg) < 0) {
                /* peer socket is dead; nothing more we can do for it */
            }
        }
        free(msg);
    }

    int left_peer = room_leave(fd);
    if (left_peer != -1) {
        ws_send_text(left_peer, "{\"type\":\"peer-left\"}");
    }
}

static void *client_thread(void *arg) {
    int fd = (int)(intptr_t)arg;

    char headers[MAX_HEADER_SIZE];
    if (read_http_headers(fd, headers, sizeof(headers)) < 0) {
        close(fd);
        return NULL;
    }

    /* Parse request line: "METHOD PATH HTTP/1.1" */
    char method[16] = {0}, path[1024] = {0};
    sscanf(headers, "%15s %1023s", method, path);

    if (strcmp(method, "GET") != 0) {
        send_status(fd, 405, "Method Not Allowed");
        close(fd);
        return NULL;
    }

    int is_ws = header_contains_ci(headers, "Upgrade", "websocket") &&
                header_contains_ci(headers, "Connection", "Upgrade");

    if (is_ws && strncmp(path, "/ws", 3) == 0) {
        char *key = header_value(headers, "Sec-WebSocket-Key");
        if (!key) {
            send_status(fd, 400, "Bad Request");
            close(fd);
            return NULL;
        }

        char concat[256];
        snprintf(concat, sizeof(concat), "%s%s", key, WS_GUID);
        free(key);

        unsigned char digest[20];
        sha1_buffer((unsigned char *)concat, strlen(concat), digest);

        char accept[BASE64_ENCODED_LEN(20)];
        base64_encode(digest, 20, accept);

        char response[512];
        int rlen = snprintf(response, sizeof(response),
            "HTTP/1.1 101 Switching Protocols\r\n"
            "Upgrade: websocket\r\n"
            "Connection: Upgrade\r\n"
            "Sec-WebSocket-Accept: %s\r\n\r\n", accept);
        if (send_all(fd, response, (size_t)rlen) < 0) {
            close(fd);
            return NULL;
        }

        char *room = query_param(path, "room");
        const char *room_name = (room && room[0]) ? room : "default";
        handle_websocket(fd, room_name);
        free(room);
    } else {
        serve_static(fd, path);
    }

    close(fd);
    return NULL;
}

/* ---------------------------------------------------------------------- */

int main(int argc, char **argv) {
    int port = DEFAULT_PORT;
    if (argc > 1) port = atoi(argv[1]);
    if (argc > 2) g_docroot = argv[2];

    for (int i = 0; i < MAX_ROOMS; i++) rooms[i].used = 0;

    signal(SIGPIPE, SIG_IGN); /* writes to a peer that hung up must not kill us */

    int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0) { perror("socket"); return 1; }

    int opt = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons((uint16_t)port);

    if (bind(listen_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind");
        return 1;
    }
    if (listen(listen_fd, 64) < 0) {
        perror("listen");
        return 1;
    }

    printf("callVideo signaling server listening on :%d (docroot: %s)\n", port, g_docroot);

    for (;;) {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);
        int client_fd = accept(listen_fd, (struct sockaddr *)&client_addr, &client_len);
        if (client_fd < 0) {
            if (errno == EINTR) continue;
            perror("accept");
            continue;
        }

        int one = 1;
        setsockopt(client_fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

        pthread_t tid;
        if (pthread_create(&tid, NULL, client_thread, (void *)(intptr_t)client_fd) != 0) {
            close(client_fd);
            continue;
        }
        pthread_detach(tid);
    }

    return 0;
}
