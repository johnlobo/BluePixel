#define _POSIX_C_SOURCE 200809L
#if defined(__APPLE__) && !defined(_DARWIN_C_SOURCE)
#define _DARWIN_C_SOURCE
#endif
#include "cpc_server.h"
#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#define MAX_CLIENTS 8
#define MAX_MESSAGE (1024u * 1024u)
#define INPUT_CAP (MAX_MESSAGE + 16384u)
#define HEADER_CAP 8192u
#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

typedef struct {
    int fd;
    bool websocket, ready, fragmented;
    unsigned char *input, *message;
    size_t used, message_size;
    time_t accepted;
} Client;

struct CPCServer {
    int unix_fd, tcp_fd, wake[2], port;
    char *unix_path, *origins;
    char token[65];
    dev_t socket_dev;
    ino_t socket_ino;
    pthread_t thread;
    atomic_bool stopping;
    CPCServerDispatch dispatch;
    void *context;
    void (*cancel)(void *context);
    Client clients[MAX_CLIENTS];
};

static int fail(char *error, size_t cap, const char *why) {
    if (error && cap) {
        snprintf(error, cap, "%s", why);
    }
    return -1;
}

static void close_fd(int *fd) {
    if (*fd >= 0) {
        close(*fd);
    }
    *fd = -1;
}

static bool valid_token(const char *s) {
    if (!s || strlen(s) != 64) {
        return false;
    }
    for (int i = 0; i < 64; i++) {
        if (!((s[i] >= '0' && s[i] <= '9') || (s[i] >= 'a' && s[i] <= 'f'))) {
            return false;
        }
    }
    return true;
}

static bool token_equal(const char *a, const char *b) {
    if (!a || strlen(a) != 64) {
        return false;
    }
    unsigned different = 0;
    for (int i = 0; i < 64; i++) {
        different |= (unsigned char)a[i] ^ (unsigned char)b[i];
    }
    return different == 0;
}

int cpc_server_token_file(const char *path, char token[65], char *error, size_t cap) {
    if (!path || !*path || !token) {
        return fail(error, cap, "token file path is required");
    }
    int fd = open(path, O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
    if (fd >= 0) {
        struct stat st;
        char data[67] = {0};
        ssize_t n;
        bool safe = fstat(fd, &st) == 0 && S_ISREG(st.st_mode) && st.st_uid == geteuid() && !(st.st_mode & 077) &&
                    st.st_nlink == 1;
        n = safe ? read(fd, data, sizeof data - 1) : -1;
        close(fd);
        if (n == 65 && data[64] == '\n') {
            data[64] = 0;
        }
        if (!safe || (n != 64 && n != 65) || !valid_token(data)) {
            return fail(error, cap, "token file must be an owner-only regular file containing 64 hex characters");
        }
        memcpy(token, data, 65);
        return 0;
    }
    if (errno != ENOENT) {
        return fail(error, cap, "cannot safely open token file");
    }
    unsigned char random[32];
    int source = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    if (source < 0) {
        return fail(error, cap, "cannot open random source");
    }
    size_t got = 0;
    while (got < sizeof random) {
        ssize_t n = read(source, random + got, sizeof random - got);
        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n <= 0) {
            break;
        }
        got += (size_t)n;
    }
    close(source);
    if (got != sizeof random) {
        return fail(error, cap, "cannot generate token");
    }
    static const char hex[] = "0123456789abcdef";
    for (int i = 0; i < 32; i++) {
        token[i * 2] = hex[random[i] >> 4];
        token[i * 2 + 1] = hex[random[i] & 15];
    }
    token[64] = 0;
    fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (fd < 0) {
        return fail(error, cap, "cannot create token file (parent directory must exist)");
    }
    char line[66];
    snprintf(line, sizeof line, "%s\n", token);
    bool ok = fchmod(fd, 0600) == 0 && write(fd, line, 65) == 65;
    close(fd);
    if (!ok) {
        unlink(path);
        return fail(error, cap, "cannot write token file");
    }
    return 0;
}

/* SHA-1 here is only RFC 6455's public handshake digest, never authentication. */
static uint32_t rol(uint32_t v, unsigned n) {
    return (v << n) | (v >> (32 - n));
}

static void websocket_accept(const char *key, char out[29]) {
    unsigned char data[128] = {0}, digest[20];
    size_t n = strlen(key);
    memcpy(data, key, n);
    memcpy(data + n, "258EAFA5-E914-47DA-95CA-C5AB0DC85B11", 36);
    n += 36;
    size_t total = (n + 9 + 63) & ~(size_t)63;
    data[n] = 0x80;
    uint64_t bits = n * 8;
    for (int i = 0; i < 8; i++) {
        data[total - 1 - i] = (unsigned char)(bits >> (i * 8));
    }
    uint32_t h[5] = {0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476, 0xc3d2e1f0};
    for (size_t off = 0; off < total; off += 64) {
        uint32_t w[80];
        for (int i = 0; i < 16; i++) {
            const unsigned char *p = data + off + i * 4;
            w[i] = (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
        }
        for (int i = 16; i < 80; i++) {
            w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
        }
        uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
        for (int i = 0; i < 80; i++) {
            uint32_t f, k;
            if (i < 20) {
                f = (b & c) | (~b & d);
                k = 0x5a827999;
            } else if (i < 40) {
                f = b ^ c ^ d;
                k = 0x6ed9eba1;
            } else if (i < 60) {
                f = (b & c) | (b & d) | (c & d);
                k = 0x8f1bbcdc;
            } else {
                f = b ^ c ^ d;
                k = 0xca62c1d6;
            }
            uint32_t t = rol(a, 5) + f + e + k + w[i];
            e = d;
            d = c;
            c = rol(b, 30);
            b = a;
            a = t;
        }
        h[0] += a;
        h[1] += b;
        h[2] += c;
        h[3] += d;
        h[4] += e;
    }
    for (int i = 0; i < 20; i++) {
        digest[i] = (unsigned char)(h[i / 4] >> (24 - (i % 4) * 8));
    }
    static const char b64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t j = 0;
    for (size_t i = 0; i < 20; i += 3) {
        unsigned v = (unsigned)digest[i] << 16;
        if (i + 1 < 20) {
            v |= (unsigned)digest[i + 1] << 8;
        }
        if (i + 2 < 20) {
            v |= digest[i + 2];
        }
        out[j++] = b64[v >> 18];
        out[j++] = b64[(v >> 12) & 63];
        out[j++] = i + 1 < 20 ? b64[(v >> 6) & 63] : '=';
        out[j++] = i + 2 < 20 ? b64[v & 63] : '=';
    }
    out[j] = 0;
}

static bool send_all(int fd, const void *data, size_t size) {
    const unsigned char *p = data;
    while (size) {
        ssize_t n = send(fd, p, size, MSG_NOSIGNAL);
        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n <= 0) {
            return false;
        }
        p += n;
        size -= (size_t)n;
    }
    return true;
}

static bool ws_send(Client *c, unsigned opcode, const void *data, size_t size) {
    unsigned char header[10];
    size_t n = 2;
    header[0] = (unsigned char)(0x80 | opcode);
    if (size < 126) {
        header[1] = (unsigned char)size;
    } else if (size <= 65535) {
        header[1] = 126;
        header[2] = (unsigned char)(size >> 8);
        header[3] = (unsigned char)size;
        n = 4;
    } else {
        header[1] = 127;
        for (int i = 0; i < 8; i++) {
            header[2 + i] = (unsigned char)((uint64_t)size >> (56 - i * 8));
        }
        n = 10;
    }
    return send_all(c->fd, header, n) && send_all(c->fd, data, size);
}

static bool ws_error(Client *c, unsigned code) {
    unsigned char body[2] = {(unsigned char)(code >> 8), (unsigned char)code};
    ws_send(c, 8, body, 2);
    return false;
}

static bool utf8(const unsigned char *s, size_t n) {
    for (size_t i = 0; i < n;) {
        unsigned v = s[i++], need, min;
        if (v < 0x80) {
            continue;
        }
        if (v >= 0xc2 && v <= 0xdf) {
            need = 1;
            min = 0x80;
            v &= 31;
        } else if (v >= 0xe0 && v <= 0xef) {
            need = 2;
            min = 0x800;
            v &= 15;
        } else if (v >= 0xf0 && v <= 0xf4) {
            need = 3;
            min = 0x10000;
            v &= 7;
        } else {
            return false;
        }
        if (n - i < need) {
            return false;
        }
        for (unsigned j = 0; j < need; j++) {
            unsigned b = s[i++];
            if ((b & 0xc0) != 0x80) {
                return false;
            }
            v = (v << 6) | (b & 63);
        }
        if (v < min || v > 0x10ffff || (v >= 0xd800 && v <= 0xdfff)) {
            return false;
        }
    }
    return true;
}

static void consume(Client *c, size_t n) {
    memmove(c->input, c->input + n, c->used - n);
    c->used -= n;
}

static bool dispatch(CPCServer *s, Client *c, const unsigned char *data, size_t n) {
    if (atomic_load(&s->stopping)) {
        return false;
    }
    if (memchr(data, 0, n) || !utf8(data, n)) {
        return c->websocket ? ws_error(c, 1007) : false;
    }
    char *request = malloc(n + 1);
    if (!request) {
        return false;
    }
    memcpy(request, data, n);
    request[n] = 0;
    char *reply = s->dispatch(s->context, request);
    free(request);
    if (!reply) {
        return false;
    }
    bool ok = c->websocket ? ws_send(c, 1, reply, strlen(reply))
                           : (send_all(c->fd, reply, strlen(reply)) && send_all(c->fd, "\n", 1));
    free(reply);
    return ok;
}

static bool list_has(const char *list, const char *wanted, bool insensitive) {
    if (!list) {
        return false;
    }
    size_t n = strlen(wanted);
    const char *p = list;
    while (*p) {
        while (*p == ',' || *p == ' ' || *p == '\t') {
            p++;
        }
        const char *end = strchr(p, ',');
        if (!end) {
            end = p + strlen(p);
        }
        const char *trim = end;
        while (trim > p && (trim[-1] == ' ' || trim[-1] == '\t')) {
            trim--;
        }
        if ((size_t)(trim - p) == n && (insensitive ? !strncasecmp(p, wanted, n) : !memcmp(p, wanted, n))) {
            return true;
        }
        p = end;
    }
    return false;
}

static bool header_key(const char *key) {
    if (!key || strlen(key) != 24 || strcmp(key + 22, "==")) {
        return false;
    }
    for (int i = 0; i < 22; i++) {
        if (!isalnum((unsigned char)key[i]) && key[i] != '+' && key[i] != '/') {
            return false;
        }
    }
    return true;
}

static bool http_error(Client *c, int code) {
    char text[160];
    int n = snprintf(text, sizeof text, "HTTP/1.1 %d Rejected\r\nConnection: close\r\nContent-Length: 0\r\n\r\n", code);
    send_all(c->fd, text, (size_t)n);
    return false;
}

static bool handshake(CPCServer *s, Client *c, size_t length) {
    char *header = malloc(length + 1);
    if (!header) {
        return false;
    }
    memcpy(header, c->input, length);
    header[length] = 0;
    if (memchr(header, 0, length)) {
        free(header);
        return http_error(c, 400);
    }
    char *line = header, *end = strstr(line, "\r\n");
    if (!end) {
        free(header);
        return false;
    }
    *end = 0;
    bool ok = !strcmp(line, "GET / HTTP/1.1") || !strcmp(line, "GET /rpc HTTP/1.1");
    char *host = NULL, *upgrade = NULL, *connection = NULL, *version = NULL, *key = NULL, *origin = NULL, *auth = NULL,
         *protocol = NULL;
    for (line = end + 2; ok && *line; line = end + 2) {
        end = strstr(line, "\r\n");
        if (!end) {
            ok = false;
            break;
        }
        *end = 0;
        if (!*line) {
            break;
        }
        char *colon = strchr(line, ':');
        if (!colon || line[0] == ' ' || line[0] == '\t') {
            ok = false;
            break;
        }
        *colon = 0;
        char *value = colon + 1;
        while (*value == ' ' || *value == '\t') {
            value++;
        }
        char *tail = value + strlen(value);
        while (tail > value && (tail[-1] == ' ' || tail[-1] == '\t')) {
            *--tail = 0;
        }
        char **slot = NULL;
        if (!strcasecmp(line, "Host")) {
            slot = &host;
        } else if (!strcasecmp(line, "Upgrade")) {
            slot = &upgrade;
        } else if (!strcasecmp(line, "Connection")) {
            slot = &connection;
        } else if (!strcasecmp(line, "Sec-WebSocket-Version")) {
            slot = &version;
        } else if (!strcasecmp(line, "Sec-WebSocket-Key")) {
            slot = &key;
        } else if (!strcasecmp(line, "Origin")) {
            slot = &origin;
        } else if (!strcasecmp(line, "Authorization")) {
            slot = &auth;
        } else if (!strcasecmp(line, "Sec-WebSocket-Protocol")) {
            slot = &protocol;
        }
        if (slot) {
            if (*slot) {
                ok = false;
                break;
            }
            *slot = value;
        }
        if (!strcasecmp(line, "Content-Length") && strcmp(value, "0")) {
            ok = false;
        }
        if (!strcasecmp(line, "Transfer-Encoding")) {
            ok = false;
        }
    }
    char local[64], localhost[64];
    snprintf(local, sizeof local, "127.0.0.1:%d", s->port);
    snprintf(localhost, sizeof localhost, "localhost:%d", s->port);
    if (!ok || !host || (strcmp(host, local) && strcmp(host, localhost)) || !upgrade ||
        strcasecmp(upgrade, "websocket") || !list_has(connection, "Upgrade", true) || !version ||
        strcmp(version, "13") || !header_key(key)) {
        free(header);
        return http_error(c, 400);
    }
    if (origin && (!strcmp(origin, "null") || !list_has(s->origins, origin, false))) {
        free(header);
        return http_error(c, 403);
    }
    char protocol_token[90];
    snprintf(protocol_token, sizeof protocol_token, "bluepixel.token.%s", s->token);
    bool authenticated = (auth && !strncmp(auth, "Bearer ", 7) && token_equal(auth + 7, s->token)) ||
                         list_has(protocol, protocol_token, false);
    if (!authenticated) {
        free(header);
        return http_error(c, 401);
    }
    char accept[29], reply[256];
    websocket_accept(key, accept);
    bool subprotocol = list_has(protocol, "bluepixel.v1", false);
    int n = snprintf(reply, sizeof reply,
                     "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: "
                     "Upgrade\r\nSec-WebSocket-Accept: %s\r\n%s\r\n",
                     accept, subprotocol ? "Sec-WebSocket-Protocol: bluepixel.v1\r\n" : "");
    free(header);
    if (!send_all(c->fd, reply, (size_t)n)) {
        return false;
    }
    consume(c, length);
    c->ready = true;
    return true;
}

static bool process_ws(CPCServer *s, Client *c) {
    if (!c->ready) {
        size_t end = 0;
        for (size_t i = 3; i < c->used; i++) {
            if (!memcmp(c->input + i - 3, "\r\n\r\n", 4)) {
                end = i + 1;
                break;
            }
        }
        if (!end) {
            return c->used < HEADER_CAP;
        }
        if (end > HEADER_CAP) {
            return http_error(c, 431);
        }
        if (!handshake(s, c, end)) {
            return false;
        }
    }
    while (c->used >= 2) {
        unsigned char *p = c->input;
        bool fin = (p[0] & 0x80) != 0;
        unsigned op = p[0] & 15;
        uint64_t size = p[1] & 127;
        size_t h = 2;
        if ((p[0] & 0x70) || !(p[1] & 0x80)) {
            return ws_error(c, 1002);
        }
        if (size == 126) {
            if (c->used < 4) {
                return true;
            }
            size = (uint64_t)p[2] << 8 | p[3];
            h = 4;
            if (size < 126) {
                return ws_error(c, 1002);
            }
        } else if (size == 127) {
            if (c->used < 10) {
                return true;
            }
            size = 0;
            for (int i = 0; i < 8; i++) {
                size = (size << 8) | p[2 + i];
            }
            h = 10;
            if (size < 65536 || (p[2] & 128)) {
                return ws_error(c, 1002);
            }
        }
        if (op >= 8 && (!fin || size > 125)) {
            return ws_error(c, 1002);
        }
        if (size > MAX_MESSAGE) {
            return ws_error(c, 1009);
        }
        if (c->used < h + 4 + (size_t)size) {
            return true;
        }
        unsigned char *body = p + h + 4;
        for (size_t i = 0; i < (size_t)size; i++) {
            body[i] ^= p[h + (i % 4)];
        }
        if (op == 8) {
            if (size == 1) {
                return ws_error(c, 1002);
            }
            if (size >= 2) {
                unsigned code = (unsigned)body[0] << 8 | body[1];
                if (!((code >= 1000 && code <= 1014 && code != 1004 && code != 1005 && code != 1006) ||
                      (code >= 3000 && code <= 4999))) {
                    return ws_error(c, 1002);
                }
                if (!utf8(body + 2, (size_t)size - 2)) {
                    return ws_error(c, 1007);
                }
            }
            ws_send(c, 8, body, (size_t)size);
            return false;
        }
        if (op == 9) {
            if (!ws_send(c, 10, body, (size_t)size)) {
                return false;
            }
        } else if (op == 10) {
        } else if (op == 2) {
            return ws_error(c, 1003);
        } else if (op == 1 || op == 0) {
            if ((op == 0 && !c->fragmented) || (op == 1 && c->fragmented)) {
                return ws_error(c, 1002);
            }
            if (c->message_size + size > MAX_MESSAGE) {
                return ws_error(c, 1009);
            }
            if (op == 1 && fin) {
                if (!dispatch(s, c, body, (size_t)size)) {
                    return false;
                }
            } else {
                if (!c->message) {
                    c->message = malloc(MAX_MESSAGE);
                    if (!c->message) {
                        return false;
                    }
                }
                memcpy(c->message + c->message_size, body, (size_t)size);
                c->message_size += (size_t)size;
                c->fragmented = !fin;
                if (fin) {
                    if (!dispatch(s, c, c->message, c->message_size)) {
                        return false;
                    }
                    c->message_size = 0;
                }
            }
        } else {
            return ws_error(c, 1002);
        }
        consume(c, h + 4 + (size_t)size);
    }
    return true;
}

static bool process_unix(CPCServer *s, Client *c) {
    for (;;) {
        unsigned char *end = memchr(c->input, '\n', c->used);
        if (!end) {
            return c->used <= (c->ready ? MAX_MESSAGE : 70);
        }
        size_t n = (size_t)(end - c->input), used = n + 1;
        if (n && c->input[n - 1] == '\r') {
            n--;
        }
        if (!c->ready) {
            char auth[71];
            if (n != 69 || memcmp(c->input, "AUTH ", 5)) {
                return false;
            }
            memcpy(auth, c->input + 5, 64);
            auth[64] = 0;
            if (!token_equal(auth, s->token)) {
                send_all(c->fd, "ERR unauthorized\n", 17);
                return false;
            }
            if (!send_all(c->fd, "OK\n", 3)) {
                return false;
            }
            c->ready = true;
        } else if (n > MAX_MESSAGE) {
            return false;
        } else if (n && !dispatch(s, c, c->input, n)) {
            return false;
        }
        consume(c, used);
    }
}

static void client_close(Client *c) {
    close_fd(&c->fd);
    free(c->input);
    free(c->message);
    memset(c, 0, sizeof *c);
    c->fd = -1;
}

static void accept_client(CPCServer *s, int listener, bool websocket) {
    int fd = accept(listener, NULL, NULL);
    if (fd < 0) {
        return;
    }
    fcntl(fd, F_SETFD, FD_CLOEXEC);
#ifdef SO_NOSIGPIPE
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#endif
    struct timeval timeout = {2, 0};
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof timeout);
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (s->clients[i].fd < 0) {
            Client *c = &s->clients[i];
            c->input = malloc(INPUT_CAP);
            if (!c->input) {
                close(fd);
                return;
            }
            c->fd = fd;
            c->websocket = websocket;
            c->accepted = time(NULL);
            return;
        }
    }
    close(fd);
}

static void *worker(void *context) {
    CPCServer *s = context;
    while (!atomic_load(&s->stopping)) {
        struct pollfd fds[MAX_CLIENTS + 3];
        int map[MAX_CLIENTS + 3], n = 0;
        fds[n] = (struct pollfd){s->wake[0], POLLIN, 0};
        map[n++] = -3;
        if (s->unix_fd >= 0) {
            fds[n] = (struct pollfd){s->unix_fd, POLLIN, 0};
            map[n++] = -1;
        }
        if (s->tcp_fd >= 0) {
            fds[n] = (struct pollfd){s->tcp_fd, POLLIN, 0};
            map[n++] = -2;
        }
        for (int i = 0; i < MAX_CLIENTS; i++) {
            if (s->clients[i].fd >= 0) {
                fds[n] = (struct pollfd){s->clients[i].fd, POLLIN, 0};
                map[n++] = i;
            }
        }
        int ready = poll(fds, (nfds_t)n, 250);
        if (ready < 0 && errno != EINTR) {
            break;
        }
        if (atomic_load(&s->stopping)) {
            break;
        }
        for (int i = 0; i < n; i++) {
            if (fds[i].revents) {
                if (map[i] == -1) {
                    accept_client(s, s->unix_fd, false);
                    continue;
                }
                if (map[i] == -2) {
                    accept_client(s, s->tcp_fd, true);
                    continue;
                }
                if (map[i] < 0) {
                    continue;
                }
                Client *c = &s->clients[map[i]];
                if (!(fds[i].revents & POLLIN)) {
                    client_close(c);
                    continue;
                }
                size_t room = INPUT_CAP - c->used;
                ssize_t got = recv(c->fd, c->input + c->used, room > 16384 ? 16384 : room, 0);
                if (got <= 0) {
                    if (got < 0 && errno == EINTR) {
                        continue;
                    }
                    client_close(c);
                    continue;
                }
                c->used += (size_t)got;
                if (!(c->websocket ? process_ws(s, c) : process_unix(s, c))) {
                    client_close(c);
                }
            }
        }
        time_t now = time(NULL);
        for (int i = 0; i < MAX_CLIENTS; i++) {
            Client *c = &s->clients[i];
            if (c->fd >= 0 && !c->ready && now - c->accepted >= 5) {
                client_close(c);
            }
        }
    }
    for (int i = 0; i < MAX_CLIENTS; i++) {
        client_close(&s->clients[i]);
    }
    return NULL;
}

static void cleanup(CPCServer *s) {
    if (!s) {
        return;
    }
    close_fd(&s->unix_fd);
    close_fd(&s->tcp_fd);
    close_fd(&s->wake[0]);
    close_fd(&s->wake[1]);
    if (s->unix_path && s->socket_ino) {
        struct stat st;
        if (lstat(s->unix_path, &st) == 0 && st.st_dev == s->socket_dev && st.st_ino == s->socket_ino) {
            unlink(s->unix_path);
        }
    }
    free(s->unix_path);
    free(s->origins);
    free(s);
}

CPCServer *cpc_server_start(const CPCServerConfig *cfg, char *error, size_t cap) {
    if (!cfg || !cfg->dispatch || !valid_token(cfg->token) || (!cfg->unix_path && cfg->websocket_port < 0) ||
        cfg->websocket_port < -1 || cfg->websocket_port > 65535) {
        fail(error, cap, "invalid server configuration");
        return NULL;
    }
    if (cfg->allowed_origins && (strchr(cfg->allowed_origins, '\r') || strchr(cfg->allowed_origins, '\n') ||
                                 strchr(cfg->allowed_origins, '*'))) {
        fail(error, cap, "Origins must be explicit values without wildcards");
        return NULL;
    }
    CPCServer *s = calloc(1, sizeof *s);
    if (!s) {
        fail(error, cap, "out of memory");
        return NULL;
    }
    s->unix_fd = s->tcp_fd = s->wake[0] = s->wake[1] = -1;
    s->port = -1;
    for (int i = 0; i < MAX_CLIENTS; i++) {
        s->clients[i].fd = -1;
    }
    atomic_init(&s->stopping, false);
    s->dispatch = cfg->dispatch;
    s->context = cfg->context;
    s->cancel = cfg->cancel;
    memcpy(s->token, cfg->token, 65);
    s->origins = strdup(cfg->allowed_origins ? cfg->allowed_origins : "");
    if (!s->origins) {
        goto failed;
    }
    if (cfg->unix_path) {
        struct sockaddr_un address = {0};
        address.sun_family = AF_UNIX;
        if (strlen(cfg->unix_path) >= sizeof address.sun_path) {
            fail(error, cap, "Unix socket path is too long");
            cleanup(s);
            return NULL;
        }
        strcpy(address.sun_path, cfg->unix_path);
        struct stat st;
        if (lstat(cfg->unix_path, &st) == 0 || errno != ENOENT) {
            fail(error, cap, "Unix socket path already exists or cannot be inspected");
            cleanup(s);
            return NULL;
        }
        s->unix_path = strdup(cfg->unix_path);
        if (!s->unix_path) {
            goto failed;
        }
        s->unix_fd = socket(AF_UNIX, SOCK_STREAM, 0);
        if (s->unix_fd < 0 || bind(s->unix_fd, (struct sockaddr *)&address, sizeof address) < 0) {
            goto failed;
        }
        if (lstat(cfg->unix_path, &st) < 0) {
            goto failed;
        }
        s->socket_dev = st.st_dev;
        s->socket_ino = st.st_ino;
        if (chmod(cfg->unix_path, 0600) < 0 || listen(s->unix_fd, MAX_CLIENTS) < 0) {
            goto failed;
        }
        fcntl(s->unix_fd, F_SETFD, FD_CLOEXEC);
    }
    if (cfg->websocket_port >= 0) {
        s->tcp_fd = socket(AF_INET, SOCK_STREAM, 0);
        if (s->tcp_fd < 0) {
            goto failed;
        }
        struct sockaddr_in address = {0};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = htons((uint16_t)cfg->websocket_port);
        if (bind(s->tcp_fd, (struct sockaddr *)&address, sizeof address) < 0 || listen(s->tcp_fd, MAX_CLIENTS) < 0) {
            goto failed;
        }
        socklen_t size = sizeof address;
        if (getsockname(s->tcp_fd, (struct sockaddr *)&address, &size) < 0) {
            goto failed;
        }
        s->port = ntohs(address.sin_port);
        fcntl(s->tcp_fd, F_SETFD, FD_CLOEXEC);
    }
    if (pipe(s->wake) < 0) {
        goto failed;
    }
    fcntl(s->wake[0], F_SETFD, FD_CLOEXEC);
    fcntl(s->wake[1], F_SETFD, FD_CLOEXEC);
    if (pthread_create(&s->thread, NULL, worker, s) != 0) {
        goto failed;
    }
    return s;
failed:
    fail(error, cap, "cannot start control server (check endpoint paths and ports)");
    cleanup(s);
    return NULL;
}

void cpc_server_stop(CPCServer *s) {
    if (!s) {
        return;
    }
    atomic_store(&s->stopping, true);
    if (s->cancel) {
        s->cancel(s->context);
    }
    /* Wake the worker poll(). A failed write (pipe already full) means a wake byte is pending, so it is ignored;
     * the variable is only there because glibc marks write() warn_unused_result and a (void) cast does not silence it. */
    ssize_t woke = write(s->wake[1], "x", 1);
    (void)woke;
    pthread_join(s->thread, NULL);
    cleanup(s);
}

int cpc_server_websocket_port(const CPCServer *s) {
    return s ? s->port : -1;
}
