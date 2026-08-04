#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>       // strncasecmp
#include <unistd.h>        // read, write, close
#include <arpa/inet.h>     // sockaddr_in, htons, etc.
#include <netdb.h>         // getaddrinfo
#include <errno.h>
#include <sys/time.h>      // struct timeval (SO_RCVTIMEO)

// ============================================================
//  Cache implementation (Weeks 1-2) — unchanged
// ============================================================

typedef struct entry {
    char *key;
    char *value;

    struct entry *hnext;   // hash bucket collision chain

    struct entry *prev;    // recency list: toward head (more recent)
    struct entry *next;    // recency list: toward tail (less recent)
} entry;

typedef struct {
    entry **buckets;
    int size;

    entry *head;           // most recently used
    entry *tail;           // least recently used

    int count;
    int capacity;
} lru_cache;

unsigned long hash(const char *key, int size) {
    unsigned long h = 5381;
    int c;
    while ((c = *key++)) {
        h = h * 33 + c;
    }
    return h % size;
}

lru_cache *cache_create(int size, int capacity) {
    lru_cache *c = malloc(sizeof(lru_cache));
    if (!c) return NULL;

    c->buckets = calloc(size, sizeof(entry *));
    if (!c->buckets) {
        free(c);
        return NULL;
    }

    c->size = size;
    c->head = NULL;
    c->tail = NULL;
    c->count = 0;
    c->capacity = capacity;
    return c;
}

void list_remove(lru_cache *c, entry *e) {
    if (e->prev) {
        e->prev->next = e->next;
    } else {
        c->head = e->next;
    }

    if (e->next) {
        e->next->prev = e->prev;
    } else {
        c->tail = e->prev;
    }

    e->prev = NULL;
    e->next = NULL;
}

void list_push_front(lru_cache *c, entry *e) {
    e->prev = NULL;
    e->next = c->head;

    if (c->head) {
        c->head->prev = e;
    } else {
        c->tail = e;
    }

    c->head = e;
}

void cache_evict(lru_cache *c) {
    entry *victim = c->tail;
    if (!victim) return;

    list_remove(c, victim);

    unsigned long idx = hash(victim->key, c->size);
    entry *cur = c->buckets[idx];
    entry *prev = NULL;
    while (cur != victim) {
        prev = cur;
        cur = cur->hnext;
    }
    if (prev == NULL) {
        c->buckets[idx] = cur->hnext;
    } else {
        prev->hnext = cur->hnext;
    }

    free(victim->key);
    free(victim->value);
    free(victim);
    c->count--;
}

char *cache_get(lru_cache *c, const char *key) {
    unsigned long idx = hash(key, c->size);

    entry *e = c->buckets[idx];
    while (e != NULL) {
        if (strcmp(e->key, key) == 0) {
            list_remove(c, e);
            list_push_front(c, e);
            return e->value;
        }
        e = e->hnext;
    }
    return NULL;
}

void cache_set(lru_cache *c, const char *key, const char *value) {
    unsigned long idx = hash(key, c->size);

    entry *e = c->buckets[idx];
    while (e != NULL) {
        if (strcmp(e->key, key) == 0) {
            free(e->value);
            e->value = strdup(value);
            list_remove(c, e);
            list_push_front(c, e);
            return;
        }
        e = e->hnext;
    }

    entry *new_entry = malloc(sizeof(entry));
    new_entry->key = strdup(key);
    new_entry->value = strdup(value);

    new_entry->hnext = c->buckets[idx];
    c->buckets[idx] = new_entry;

    list_push_front(c, new_entry);
    c->count++;

    if (c->count > c->capacity) {
        cache_evict(c);
    }
}

void cache_free(lru_cache *c) {
    entry *e = c->head;
    while (e != NULL) {
        entry *next = e->next;
        free(e->key);
        free(e->value);
        free(e);
        e = next;
    }
    free(c->buckets);
    free(c);
}

// ============================================================
//  Server / reverse proxy (Week 3) + keep-alive, both sides
// ============================================================

#define PORT 8080
#define BUFFER_SIZE 8192

#define ORIGIN_HOST "localhost"
#define ORIGIN_PORT "80"          // nginx

#define BACKLOG 128               // the accept queue IS the waiting line. At 10,
                                  // wrk -c50 gets connections refused before the
                                  // server is even the bottleneck.

// How long we'll sit waiting for the next request on an idle kept-alive
// connection before giving up on it. This is what stops keep-alive from
// leaking connections: a client that stops talking without closing gets
// timed out rather than held forever.
#define IDLE_TIMEOUT_SEC 15

// Timeout on the ORIGIN socket. Mandatory now that we ask nginx to keep the
// connection open: nginx will no longer close to signal "done", so a bug in
// our framing would otherwise block here until nginx's keepalive_timeout
// (75s by default) expires.
#define ORIGIN_TIMEOUT_SEC 5

// Cap on requests served per connection. Without a cap, a single-threaded
// server that adopts keep-alive will serve ONE client forever and never return
// to accept() — see the note in main().
#define MAX_KEEPALIVE_REQUESTS 100

void parse_path(const char *request, char *out, size_t out_size) {
    char fmt[32];
    snprintf(fmt, sizeof(fmt), "%%*s %%%zus", out_size - 1);

    out[0] = '\0';
    if (sscanf(request, fmt, out) != 1) {
        strncpy(out, "/", out_size);
        out[out_size - 1] = '\0';
    }
}

// write-all helper: write() may accept fewer bytes than asked under load.
// Returns 0 on success, -1 if the peer went away mid-write — which the
// keep-alive loop needs to know so it can stop instead of reading again.
static int write_all(int fd, const char *buf, size_t len) {
    size_t total = 0;
    while (total < len) {
        ssize_t w = write(fd, buf + total, len - total);
        if (w <= 0) return -1;   // error, timeout, or peer closed
        total += w;
    }
    return 0;
}

// Case-insensitive substring search over a bounded region (headers only —
// never let one of these run into the body, which can contain anything).
static const char *find_ci(const char *hay, size_t n, const char *needle) {
    size_t m = strlen(needle);
    if (m > n) return NULL;
    for (size_t i = 0; i + m <= n; i++) {
        if (strncasecmp(hay + i, needle, m) == 0) return hay + i;
    }
    return NULL;
}

// ------------------------------------------------------------
//  Origin connection — persistent, HTTP/1.1 keep-alive
// ------------------------------------------------------------
// Single-threaded server, so one reusable origin socket is enough. Reusing it
// is the entire point of asking nginx for keep-alive: otherwise we'd send
// "Connection: keep-alive" and then hang up anyway, which buys nothing.

static int g_origin_fd = -1;

static void origin_drop(void) {
    if (g_origin_fd >= 0) { close(g_origin_fd); g_origin_fd = -1; }
}

static int origin_connect(void) {
    struct addrinfo hints, *res;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family   = AF_INET;       // IPv4
    hints.ai_socktype = SOCK_STREAM;   // TCP

    if (getaddrinfo(ORIGIN_HOST, ORIGIN_PORT, &hints, &res) != 0) {
        perror("getaddrinfo");
        return -1;
    }

    int fd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (fd < 0) { perror("socket"); freeaddrinfo(res); return -1; }

    if (connect(fd, res->ai_addr, res->ai_addrlen) < 0) {
        perror("connect");
        close(fd);
        freeaddrinfo(res);
        return -1;
    }
    freeaddrinfo(res);

    struct timeval tv = { .tv_sec = ORIGIN_TIMEOUT_SEC, .tv_usec = 0 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    return fd;
}

// One attempt on the current g_origin_fd.
//   >0  bytes of a complete response in `out`
//   -1  the socket looks stale and nothing was received — safe to retry fresh
//   -2  a real failure — do not retry
static ssize_t origin_fetch_once(const char *path, char *out, size_t out_size) {
    char request[2048];
    int req_len = snprintf(request, sizeof(request),
        "GET %s HTTP/1.1\r\n"
        "Host: %s\r\n"
        "Connection: keep-alive\r\n"
        "\r\n",
        path, ORIGIN_HOST);
    if (req_len < 0 || (size_t)req_len >= sizeof(request)) return -2;

    if (write_all(g_origin_fd, request, (size_t)req_len) < 0) return -1;

    // --- read until the end of the header block ---
    // We can no longer read until EOF: nginx holds the connection open, so
    // there is no EOF. Content-Length is what tells us where the body ends.
    size_t total = 0;
    char *hdr_end = NULL;

    while (!hdr_end) {
        if (total >= out_size - 1) return -2;          // headers larger than our buffer
        ssize_t r = read(g_origin_fd, out + total, out_size - 1 - total);
        if (r <= 0) {
            // Nothing received yet => this was almost certainly a keep-alive
            // socket nginx had already timed out. Retryable.
            return -1;   // EOF mid-headers: recycled keepalive conn, retryable
        }
        total += (size_t)r;
        out[total] = '\0';
        hdr_end = strstr(out, "\r\n\r\n");
    }
    size_t header_len = (size_t)(hdr_end - out) + 4;   // includes the blank line

    const char *cl = find_ci(out, header_len, "\r\ncontent-length:");
    if (!cl) {
        // No Content-Length (chunked, or a response type we don't handle). With
        // a persistent connection there's no way to find the end of the body,
        // so refuse rather than guess. nginx sends Content-Length for static
        // files, so this is the "something unexpected" path.
        return -2;
    }
    long long body_len = strtoll(cl + strlen("\r\ncontent-length:"), NULL, 10);
    if (body_len < 0) return -2;

    size_t need = header_len + (size_t)body_len;
    if (need > out_size - 1) return -2;                // response too big to cache

    // --- read exactly the rest of the body, no further ---
    // Reading past `need` would swallow the front of the NEXT response on this
    // reused connection and desynchronise everything after it.
    while (total < need) {
        ssize_t r = read(g_origin_fd, out + total, need - total);
        if (r <= 0) return -2;
        total += (size_t)r;
    }
    out[total] = '\0';

    // If the origin says it's closing anyway, don't try to reuse the socket.
    if (find_ci(out, header_len, "connection: close")) origin_drop();

    return (ssize_t)total;
}

// Fetch `path` from the origin, reusing the persistent connection when it's
// alive. Returns response length, or -1.
ssize_t fetch_from_origin(const char *path, char *response_out, size_t out_size) {
    for (int attempt = 0; attempt < 2; attempt++) {
        if (g_origin_fd < 0) {
            g_origin_fd = origin_connect();
            if (g_origin_fd < 0) return -1;
        }

        ssize_t r = origin_fetch_once(path, response_out, out_size);
        if (r > 0) return r;

        origin_drop();
        if (r == -2) return -1;    // real failure — retrying won't help
        // r == -1: stale pooled socket. Loop once more with a fresh connection.
        // Safe because nothing of the response arrived, and GET is idempotent.
    }
    return -1;
}

// ------------------------------------------------------------
//  Response normalization
// ------------------------------------------------------------
// Connection (and friends) are HOP-BY-HOP headers: they describe one TCP link,
// not the end-to-end message. A proxy that relays them verbatim is passing the
// origin's opinion about the origin link on to the client — which is exactly
// how "Connection: close" from nginx ended up telling wrk to hang up. Worse,
// caching that header would have served the close directive forever.
//
// So: strip them, and state our own.

static int is_hop_by_hop(const char *line, size_t len) {
    static const char *names[] = {
        "connection:", "keep-alive:", "proxy-connection:",
        "transfer-encoding:", "upgrade:", "te:", "trailer:", NULL
    };
    for (int i = 0; names[i]; i++) {
        size_t n = strlen(names[i]);
        if (len >= n && strncasecmp(line, names[i], n) == 0) return 1;
    }
    return 0;
}

// Rewrites `resp` in place. Returns the new length, or -1 if unusable.
static ssize_t normalize_response(char *resp, size_t len, size_t cap) {
    char *hdr_end = strstr(resp, "\r\n\r\n");
    if (!hdr_end) return -1;

    size_t header_block = (size_t)(hdr_end - resp) + 2;   // through the last header's CRLF
    size_t body_off     = header_block + 2;
    size_t body_len     = len - body_off;

    char tmp[BUFFER_SIZE * 4];
    size_t w = 0;
    size_t i = 0;
    int first = 1;   // the status line is not a header — never drop it

    while (i < header_block) {
        size_t j = i;
        while (j + 1 < header_block && !(resp[j] == '\r' && resp[j + 1] == '\n')) j++;
        size_t line_len = j - i;

        if (first || !is_hop_by_hop(resp + i, line_len)) {
            if (w + line_len + 2 > sizeof(tmp)) return -1;
            memcpy(tmp + w, resp + i, line_len);
            w += line_len;
            tmp[w++] = '\r';
            tmp[w++] = '\n';
        }
        first = 0;
        i = j + 2;
    }

    static const char conn_hdr[] = "Connection: keep-alive\r\n\r\n";
    if (w + sizeof(conn_hdr) - 1 + body_len > sizeof(tmp)) return -1;
    memcpy(tmp + w, conn_hdr, sizeof(conn_hdr) - 1);
    w += sizeof(conn_hdr) - 1;

    memcpy(tmp + w, resp + body_off, body_len);
    w += body_len;

    if (w + 1 > cap) return -1;
    memcpy(resp, tmp, w);
    resp[w] = '\0';
    return (ssize_t)w;
}

// A connection can only be reused if the client can tell where the response
// ends without waiting for EOF: Content-Length present, no Connection: close.

// An HTTP/1.0 response with no explicit keep-alive means "closes when done" to
// every client, wrk included — relay that and the client hangs up after every
// response. One-byte edit, since the version tokens are the same length.
static void upgrade_to_http11(char *resp) {
    if (strncmp(resp, "HTTP/1.0", 8) == 0) resp[7] = '1';
}

// Did the CLIENT ask us to close? Its request is the authority on the client
// link, the same way our request is the authority on the origin link.
static int client_requested_close(const char *req, size_t len) {
    const char *end = strstr(req, "\r\n\r\n");
    size_t hlen = end ? (size_t)(end - req) + 2 : len;
    if (find_ci(req, hlen, "\r\nconnection: close")) return 1;
    // HTTP/1.0 clients default to close unless they explicitly ask otherwise.
    const char *eol = strstr(req, "\r\n");
    size_t line = eol ? (size_t)(eol - req) : len;
    if (find_ci(req, line, "HTTP/1.0") && !find_ci(req, hlen, "connection: keep-alive")) return 1;
    return 0;
}

int main(void) {
    lru_cache *cache = cache_create(1024, 100);
    if (!cache) { fprintf(stderr, "failed to create cache\n"); exit(1); }

    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) { perror("socket"); exit(1); }

    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr;
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(PORT);
    if (bind(server_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind"); exit(1);
    }

    if (listen(server_fd, BACKLOG) < 0) { perror("listen"); exit(1); }
    printf("Proxy listening on port %d, forwarding to %s:%s (keep-alive: client + origin)\n",
           PORT, ORIGIN_HOST, ORIGIN_PORT);

    while (1) {
        int client_fd = accept(server_fd, NULL, NULL);
        if (client_fd < 0) { perror("accept"); continue; }

        // Idle timeout on the accepted socket. With keep-alive we now block in
        // read() waiting for a request that may never come; without this, one
        // silent client wedges the entire server forever. SO_SNDTIMEO does the
        // same for a client that stops draining its receive window mid-response.
        struct timeval tv = { .tv_sec = IDLE_TIMEOUT_SEC, .tv_usec = 0 };
        setsockopt(client_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        setsockopt(client_fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

        // ---- keep-alive loop: many requests on this one connection ----
        int served = 0;
        while (served < MAX_KEEPALIVE_REQUESTS) {
            char buffer[BUFFER_SIZE];

            // Framing simplification: we assume one read() yields exactly one
            // complete request. True in practice for wrk's small GETs on a LAN,
            // not true in general (a request can span reads, or two can arrive
            // together). The epoll version buffers properly.
            ssize_t n = read(client_fd, buffer, BUFFER_SIZE - 1);
            if (n <= 0) {
                // n == 0 : client closed cleanly.
                // n <  0 : error, or EAGAIN/EWOULDBLOCK from SO_RCVTIMEO —
                //          i.e. the client went idle. Either way we're done.
                break;
            }
            buffer[n] = '\0';

            int wants_close = client_requested_close(buffer, (size_t)n);

            char key[2048];
            parse_path(buffer, key, sizeof(key));

            char origin_response[BUFFER_SIZE * 4];   // scratch for a miss fetch
            char *raw_response;
            size_t raw_len;
            int keep_alive;

            char *cached = cache_get(cache, key);
            if (cached) {
                // HIT: cached value is a complete raw HTTP response that was
                // already normalized before it went into the cache.
                raw_response = cached;
                raw_len = strlen(cached);
                keep_alive = !wants_close;
                //printf("HIT  [%s]\n", key);
            } else {
                ssize_t olen = fetch_from_origin(key, origin_response, sizeof(origin_response));
                if (olen > 0) {
                    origin_response[olen] = '\0';
                    olen = normalize_response(origin_response, (size_t)olen,
                                              sizeof(origin_response));
                }

                if (olen > 0) {
                    upgrade_to_http11(origin_response);
                    cache_set(cache, key, origin_response);   // cache the normalized form
                    raw_response = origin_response;
                    raw_len = (size_t)olen;
                    keep_alive = !wants_close;
                } else {
                    // origin unreachable or unusable — gateway error, then hang up.
                    // NOTE: string literal, read-only. Never pass it to
                    // upgrade_to_http11(), which writes in place.
                    raw_response =
                        "HTTP/1.1 502 Bad Gateway\r\n"
                        "Content-Type: text/plain\r\n"
                        "Content-Length: 16\r\n"
                        "Connection: close\r\n"
                        "\r\n"
                        "502 Bad Gateway\n";
                    raw_len = strlen(raw_response);
                    keep_alive = 0;
                }
                //printf("MISS [%s]\n", key);
            }

            if (write_all(client_fd, raw_response, raw_len) < 0) break;

            served++;
            if (!keep_alive || wants_close) break;
            // no close here: loop back and read the next request
        }

        close(client_fd);   // closed once, after the client is done with us
    }

    origin_drop();
    cache_free(cache);
    close(server_fd);
    return 0;
}