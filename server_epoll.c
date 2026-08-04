// ============================================================
//  server_epoll.c — single-threaded epoll event loop with a
//                   NON-BLOCKING origin fetch and client-side
//                   HTTP keep-alive.
//
//  One thread. One loop. Two sockets per cache miss (client +
//  origin), both registered with the same epoll instance and
//  both hanging off the same `connection` object.
//
//  build: gcc -Wall -Wextra -O2 -o server_epoll server_epoll.c
// ============================================================

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>        // strncasecmp
#include <stdint.h>
#include <unistd.h>         // read, write, close
#include <arpa/inet.h>      // sockaddr_in, htons
#include <netdb.h>          // getaddrinfo (startup only)
#include <fcntl.h>          // fcntl, O_NONBLOCK
#include <sys/epoll.h>      // epoll_create1, epoll_ctl, epoll_wait
#include <sys/socket.h>     // getsockopt, SO_ERROR
#include <errno.h>

// ============================================================
//  Cache implementation (Weeks 1-2) — verbatim from server_pool.c,
//  minus the mutex. Single thread means no shared-state races.
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

    entry *head;
    entry *tail;

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
    if (!new_entry) return;
    new_entry->key = strdup(key);
    new_entry->value = strdup(value);
    if (!new_entry->key || !new_entry->value) {
        free(new_entry->key);
        free(new_entry->value);
        free(new_entry);
        return;
    }

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
//  Event-loop server layer
// ============================================================

#define PORT         8080
#define BUFFER_SIZE  8192
#define MAX_EVENTS   64

#define ORIGIN_HOST  "localhost"
#define ORIGIN_PORT  "80"

// Per-request logging costs a syscall (and, to a terminal, far more than the
// request itself). Keep at 0 for benchmarks.
#define LOG_REQUESTS 0

#if LOG_REQUESTS
#define LOGF(...) printf(__VA_ARGS__)
#else
#define LOGF(...) ((void)0)
#endif

// Resolved once at startup. getaddrinfo() can block on DNS, and a blocking
// call anywhere in the loop defeats the whole design — so we do it before the
// loop starts and reuse the sockaddr for every outbound connect().
static struct sockaddr_in origin_addr;

// ------------------------------------------------------------
//  Per-connection state machine
// ------------------------------------------------------------
typedef enum {
    ST_READING,            // reading the client's request
    ST_ORIGIN_CONNECTING,  // non-blocking connect() to origin in flight
    ST_ORIGIN_SENDING,     // flushing the GET request to the origin
    ST_ORIGIN_READING,     // reading the origin's response until EOF
    ST_WRITING             // writing the response back to the client
} conn_state;

struct connection;

// What we hang off each registered fd. epoll's data field is a UNION — we use
// .ptr, so we cannot also stash the fd in .fd. And we can't infer which socket
// fired from the state alone, because the client fd stays registered while
// we're talking to the origin. So each connection owns two tags, and the tag
// tells us both which connection and which side of it woke up.
typedef struct {
    struct connection *conn;
    int is_origin;
} ev_tag;

typedef struct connection {
    int fd;                      // client socket
    int origin_fd;               // origin socket, -1 when none open
    conn_state state;

    ev_tag client_tag;           // registered with fd
    ev_tag origin_tag;           // registered with origin_fd

    char key[2048];              // request path — the cache key. Saved at parse
                                 // time because buf gets overwritten.

    char buf[BUFFER_SIZE * 4];   // client request in, then the response out.
                                 // Reused for the origin's response: once the
                                 // path is parsed the request bytes are dead,
                                 // so the origin reads land exactly where the
                                 // client write will read from. No extra copy.
    size_t len;                  // bytes in buf
    size_t off;                  // bytes already written to the client

    int keep_alive;              // reuse this connection after the response?

    // Bytes the client sent AFTER the request we're currently serving. buf is
    // about to be overwritten by the response, so they have to live somewhere
    // else until we flip back to reading. Almost always empty — a client has
    // to pipeline (or send early) for this to be non-zero.
    char pend[BUFFER_SIZE];
    size_t pend_len;

    char oreq[2304];             // the GET we send to the origin
    size_t oreq_len;
    size_t oreq_off;             // resumable partial write
} connection;

static int set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) return -1;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

static void epoll_add(int epfd, int fd, uint32_t events, void *ptr) {
    struct epoll_event ev;
    ev.events = events;
    ev.data.ptr = ptr;
    if (epoll_ctl(epfd, EPOLL_CTL_ADD, fd, &ev) < 0) perror("epoll_ctl ADD");
}

static void epoll_mod(int epfd, int fd, uint32_t events, void *ptr) {
    struct epoll_event ev;
    ev.events = events;
    ev.data.ptr = ptr;
    if (epoll_ctl(epfd, EPOLL_CTL_MOD, fd, &ev) < 0) perror("epoll_ctl MOD");
}

// Tear down just the origin side, leaving the client connection alive.
static void origin_close(int epfd, connection *c) {
    if (c->origin_fd < 0) return;
    epoll_ctl(epfd, EPOLL_CTL_DEL, c->origin_fd, NULL);
    close(c->origin_fd);
    c->origin_fd = -1;
}

// Tear down everything. FREES c — the caller must not touch it afterwards.
static void conn_close(int epfd, connection *c) {
    origin_close(epfd, c);
    epoll_ctl(epfd, EPOLL_CTL_DEL, c->fd, NULL);
    close(c->fd);
    free(c);
}

void parse_path(const char *request, char *out, size_t out_size) {
    char fmt[32];
    snprintf(fmt, sizeof(fmt), "%%*s %%%zus", out_size - 1);

    out[0] = '\0';
    if (sscanf(request, fmt, out) != 1) {
        strncpy(out, "/", out_size);
        out[out_size - 1] = '\0';
    }
}

// ------------------------------------------------------------
//  Header helpers
// ------------------------------------------------------------

// Case-insensitive substring search over a bounded region (headers only —
// never let one of these run into a body, which can contain anything).
static const char *find_ci(const char *hay, size_t n, const char *needle) {
    size_t m = strlen(needle);
    if (m > n) return NULL;
    for (size_t i = 0; i + m <= n; i++) {
        if (strncasecmp(hay + i, needle, m) == 0) return hay + i;
    }
    return NULL;
}

// Did the CLIENT ask us to close? HTTP/1.1 defaults to keep-alive; HTTP/1.0
// defaults the other way.
static int client_requested_close(const char *req, size_t len) {
    if (find_ci(req, len, "\r\nconnection: close")) return 1;
    const char *eol = strstr(req, "\r\n");
    size_t line = eol ? (size_t)(eol - req) : len;
    if (find_ci(req, line, "HTTP/1.0") && !find_ci(req, len, "connection: keep-alive")) return 1;
    return 0;
}

// Connection (and friends) are HOP-BY-HOP headers: they describe one TCP link,
// not the end-to-end message. We speak HTTP/1.0 to the origin, so the origin
// answers "Connection: close" — relay that verbatim and we'd be telling the
// CLIENT to hang up after every response, and caching it would serve that
// close directive forever. So: strip them, and state our own.
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

    size_t header_block = (size_t)(hdr_end - resp) + 2;   // through last header's CRLF
    size_t body_off     = header_block + 2;
    size_t body_len     = len - body_off;

    static char tmp[BUFFER_SIZE * 4];   // single-threaded: one scratch buffer is fine
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

    if (w > cap) return -1;
    memcpy(resp, tmp, w);
    resp[w] = '\0';
    return (ssize_t)w;
}

// A connection can only be reused if the client can find the end of the
// response without waiting for EOF: Content-Length present, no Connection: close.
static int can_keep_alive(const char *resp) {
    const char *end = strstr(resp, "\r\n\r\n");
    if (!end) return 0;
    size_t hlen = (size_t)(end - resp) + 2;

    if (!find_ci(resp, hlen, "\r\ncontent-length:")) return 0;
    if (find_ci(resp, hlen, "connection: close"))    return 0;
    return 1;
}

// An HTTP/1.0 response with no explicit keep-alive means "closes when done" to
// every client. One-byte edit, since the version tokens are the same length.
static void upgrade_to_http11(char *resp) {
    if (strncmp(resp, "HTTP/1.0", 8) == 0) resp[7] = '1';
}

// ------------------------------------------------------------

static void on_writable(int epfd, connection *c, lru_cache *cache);

// buf already holds the response (len set). Start sending it.
static void flip_to_writing(int epfd, connection *c) {
    c->off = 0;
    c->state = ST_WRITING;
    // Switch what we listen for. Staying on EPOLLIN would mean epoll never
    // reports the socket writable and the connection hangs forever.
    epoll_mod(epfd, c->fd, EPOLLOUT, &c->client_tag);
}

// Copy a response into buf, then start sending it.
static void begin_response(int epfd, connection *c, const char *resp, size_t rlen) {
    if (rlen > sizeof(c->buf) - 1) rlen = sizeof(c->buf) - 1;
    memmove(c->buf, resp, rlen);      // memmove: resp may alias c->buf
    c->buf[rlen] = '\0';
    c->len = rlen;
    flip_to_writing(epfd, c);
}

static const char RESP_502[] =
    "HTTP/1.1 502 Bad Gateway\r\n"
    "Content-Type: text/plain\r\n"
    "Content-Length: 16\r\n"
    "Connection: close\r\n"
    "\r\n"
    "502 Bad Gateway\n";

static const char RESP_400[] =
    "HTTP/1.1 400 Bad Request\r\n"
    "Content-Type: text/plain\r\n"
    "Content-Length: 16\r\n"
    "Connection: close\r\n"
    "\r\n"
    "400 Bad Request\n";

// Abandon the origin and send an error to the client, then hang up.
// MAY FREE c (via on_writable) — every caller must `return` straight after.
static void fail_with(int epfd, connection *c, const char *resp, size_t rlen,
                      lru_cache *cache) {
    origin_close(epfd, c);
    c->keep_alive = 0;                // these responses say Connection: close
    c->pend_len = 0;                  // whatever was pipelined is moot now
    begin_response(epfd, c, resp, rlen);
    on_writable(epfd, c, cache);
}

// ------------------------------------------------------------
//  Origin fetch — step 1: start a non-blocking connect
// ------------------------------------------------------------
// connect() on a non-blocking socket cannot wait for the TCP handshake, so it
// returns immediately with EINPROGRESS. We register the socket for EPOLLOUT:
// writable means the handshake finished — successfully OR not, which is why
// step 2 has to check SO_ERROR rather than assuming success.
//
// MAY FREE c on the failure paths. Caller must return immediately.
static void start_origin_fetch(int epfd, connection *c, lru_cache *cache) {
    int ofd = socket(AF_INET, SOCK_STREAM, 0);
    if (ofd < 0) { fail_with(epfd, c, RESP_502, sizeof(RESP_502) - 1, cache); return; }

    if (set_nonblocking(ofd) < 0) {
        close(ofd);
        fail_with(epfd, c, RESP_502, sizeof(RESP_502) - 1, cache);
        return;
    }

    int r = connect(ofd, (struct sockaddr *)&origin_addr, sizeof(origin_addr));
    if (r < 0 && errno != EINPROGRESS && errno != EINTR) {
        // EINTR on a non-blocking connect also means "in progress" — don't
        // retry it, that would return EALREADY.
        close(ofd);
        fail_with(epfd, c, RESP_502, sizeof(RESP_502) - 1, cache);
        return;
    }

    // HTTP/1.0 to the origin, deliberately: the origin then closes when it's
    // done, and that EOF is what tells ST_ORIGIN_READING the body has ended.
    // Origin-side keep-alive would need Content-Length framing instead — see
    // the note at the bottom of this file.
    int n = snprintf(c->oreq, sizeof(c->oreq),
                     "GET %s HTTP/1.0\r\n"
                     "Host: %s\r\n"
                     "\r\n",
                     c->key, ORIGIN_HOST);
    if (n < 0) n = 0;
    if ((size_t)n >= sizeof(c->oreq)) n = (int)sizeof(c->oreq) - 1;
    c->oreq_len = (size_t)n;
    c->oreq_off = 0;

    c->len = 0;                 // buf now accumulates the ORIGIN's response
    c->off = 0;
    c->origin_fd = ofd;
    c->state = ST_ORIGIN_CONNECTING;

    // Stop watching the client for readability. It has nothing more to say
    // until we answer, and under level-triggered epoll an event we register
    // for but never handle is reported on every single epoll_wait — that's a
    // 100%-CPU spin. Requesting 0 events still delivers EPOLLERR/EPOLLHUP,
    // which is exactly what we want: if the client vanishes mid-fetch we hear
    // about it and can tear the whole thing down.
    epoll_mod(epfd, c->fd, 0, &c->client_tag);

    epoll_add(epfd, ofd, EPOLLOUT, &c->origin_tag);
}

// ------------------------------------------------------------
//  Serve one complete request that is already buffered in c->buf
// ------------------------------------------------------------
// Returns 1 if a response is now staged in buf and the connection is in
// ST_WRITING; 0 if it went async (origin fetch) or was freed.
static int process_request(int epfd, connection *c, lru_cache *cache) {
    char *req_end = strstr(c->buf, "\r\n\r\n");
    size_t req_len = req_end ? (size_t)(req_end - c->buf) + 4 : c->len;

    parse_path(c->buf, c->key, sizeof(c->key));
    c->keep_alive = !client_requested_close(c->buf, req_len);

    // Stash anything the client already sent past this request — the response
    // is about to overwrite buf. This is the whole reason `pend` exists.
    size_t leftover = c->len > req_len ? c->len - req_len : 0;
    if (leftover > sizeof(c->pend)) {
        // More pipelined data than we're willing to hold. Serve this request,
        // then hang up rather than silently dropping the rest.
        c->keep_alive = 0;
        leftover = 0;
    }
    if (leftover > 0) memcpy(c->pend, c->buf + req_len, leftover);
    c->pend_len = leftover;
    c->len = 0;

    char *cached = cache_get(cache, c->key);
    if (cached) {
        // Copy out immediately. `cached` points into the cache, and this
        // connection lives across many loop iterations — another connection's
        // cache_set (or the eviction it triggers) can free that memory before
        // we finish writing. Same rule as the mutex versions, different reason.
        LOGF("HIT  %s\n", c->key);
        size_t vlen = strlen(cached);
        if (!can_keep_alive(cached)) c->keep_alive = 0;
        begin_response(epfd, c, cached, vlen);
        return 1;
    }

    LOGF("MISS %s\n", c->key);
    start_origin_fetch(epfd, c, cache);
    return 0;   // the client now waits, tracked by state, while others are served
}

// ------------------------------------------------------------
//  Client socket READABLE (state ST_READING)
// ------------------------------------------------------------
// MAY FREE c. Caller must return immediately.
static void on_readable(int epfd, connection *c, lru_cache *cache) {
    size_t space = sizeof(c->buf) - 1 - c->len;   // spare byte for the NUL

    if (space > 0) {
        ssize_t n;
        do {
            n = read(c->fd, c->buf + c->len, space);
        } while (n < 0 && errno == EINTR);

        if (n > 0) {
            c->len += (size_t)n;
        } else if (n == 0) {
            conn_close(epfd, c);          // client left between/mid request
            return;
        } else if (errno != EAGAIN && errno != EWOULDBLOCK) {
            conn_close(epfd, c);
            return;
        }
        // EAGAIN: not an error. Fall through — what we already buffered may
        // well be a complete request.
    }

    c->buf[c->len] = '\0';

    // A request ends at the header terminator. Under keep-alive we need the
    // real boundary, not just the end of the request line: without it we
    // couldn't tell where request N stops and request N+1 begins.
    if (!strstr(c->buf, "\r\n\r\n")) {
        if (c->len >= sizeof(c->buf) - 1) {
            fail_with(epfd, c, RESP_400, sizeof(RESP_400) - 1, cache);
            return;
        }
        return;                            // partial request — wait for more
    }

    if (!process_request(epfd, c, cache)) return;
    on_writable(epfd, c, cache);           // usually writable already
}

// ------------------------------------------------------------
//  Client socket WRITABLE (state ST_WRITING)
// ------------------------------------------------------------
// Sends the staged response, then either reuses the connection for the next
// request or closes it. Loops rather than recursing: a client that pipelines N
// requests would otherwise nest N frames deep through
// on_writable -> flip -> process_request -> on_writable.
//
// MAY FREE c. Caller must return immediately.
static void on_writable(int epfd, connection *c, lru_cache *cache) {
    for (;;) {
        while (c->off < c->len) {
            ssize_t n = write(c->fd, c->buf + c->off, c->len - c->off);

            if (n > 0) {
                c->off += (size_t)n;       // partial write: resume from here
                continue;
            }
            if (n < 0 && errno == EINTR) continue;
            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                return;                    // send buffer full; wait for EPOLLOUT
            }
            conn_close(epfd, c);           // EPIPE, ECONNRESET, ...
            return;
        }

        // ---- response fully sent ----
        if (!c->keep_alive) {
            conn_close(epfd, c);
            return;
        }

        // Reuse the connection: restore whatever the client sent past the
        // request we just served.
        c->len = c->pend_len;
        if (c->pend_len > 0) memcpy(c->buf, c->pend, c->pend_len);
        c->pend_len = 0;
        c->off = 0;
        c->buf[c->len] = '\0';
        c->state = ST_READING;

        // A complete pipelined request already in hand? Serve it now —
        // level-triggered epoll will not re-notify us about bytes that are
        // already off the socket and sitting in our buffer.
        if (c->len > 0 && strstr(c->buf, "\r\n\r\n")) {
            if (!process_request(epfd, c, cache)) return;   // async or freed
            continue;                                       // loop writes the response
        }

        epoll_mod(epfd, c->fd, EPOLLIN, &c->client_tag);
        return;
    }
}

// ------------------------------------------------------------
//  Origin response complete (EOF from origin)
// ------------------------------------------------------------
// MAY FREE c. Caller must return immediately.
static void origin_finish(int epfd, connection *c, lru_cache *cache) {
    origin_close(epfd, c);

    if (c->len == 0) {                     // origin closed without answering
        fail_with(epfd, c, RESP_502, sizeof(RESP_502) - 1, cache);
        return;
    }

    c->buf[c->len] = '\0';

    // Strip the origin's hop-by-hop headers and state our own. Without this we
    // would relay the origin's "Connection: close" to the client and keep-alive
    // would never engage — and the cached copy would carry it forever.
    ssize_t nl = normalize_response(c->buf, c->len, sizeof(c->buf) - 1);
    if (nl <= 0) {
        fail_with(epfd, c, RESP_502, sizeof(RESP_502) - 1, cache);
        return;
    }
    c->len = (size_t)nl;
    upgrade_to_http11(c->buf);

    cache_set(cache, c->key, c->buf);      // cache the NORMALIZED form

    if (!can_keep_alive(c->buf)) c->keep_alive = 0;

    // buf already holds the response, in place — nothing to copy.
    flip_to_writing(epfd, c);
    on_writable(epfd, c, cache);
}

// ------------------------------------------------------------
//  Origin socket events — the dual-FD half of the state machine
// ------------------------------------------------------------
// MAY FREE c. Caller must return immediately.
static void on_origin_event(int epfd, connection *c, uint32_t e, lru_cache *cache) {

    // ---- step 2: the connect finished (or failed) ----
    if (c->state == ST_ORIGIN_CONNECTING) {
        if (e & EPOLLERR) {
            fail_with(epfd, c, RESP_502, sizeof(RESP_502) - 1, cache);
            return;
        }

        // Writable means the handshake COMPLETED, not that it SUCCEEDED. A
        // refused connection also wakes us up writable. SO_ERROR is where the
        // kernel left the verdict; 0 means genuinely connected.
        int soerr = 0;
        socklen_t slen = sizeof(soerr);
        if (getsockopt(c->origin_fd, SOL_SOCKET, SO_ERROR, &soerr, &slen) < 0 || soerr != 0) {
            errno = soerr;
            fail_with(epfd, c, RESP_502, sizeof(RESP_502) - 1, cache);
            return;
        }

        c->state = ST_ORIGIN_SENDING;      // fall through and try sending now
    }

    // ---- step 3: flush the GET request to the origin ----
    if (c->state == ST_ORIGIN_SENDING) {
        while (c->oreq_off < c->oreq_len) {
            ssize_t n = write(c->origin_fd, c->oreq + c->oreq_off,
                              c->oreq_len - c->oreq_off);
            if (n > 0) { c->oreq_off += (size_t)n; continue; }
            if (n < 0 && errno == EINTR) continue;
            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                return;                    // resume on the next EPOLLOUT
            }
            fail_with(epfd, c, RESP_502, sizeof(RESP_502) - 1, cache);
            return;
        }

        // Request fully sent. Now we care about the origin being READABLE,
        // not writable — same event-switching rule as the client side.
        c->state = ST_ORIGIN_READING;
        epoll_mod(epfd, c->origin_fd, EPOLLIN, &c->origin_tag);
        return;
    }

    // ---- step 4: read the origin's response until EOF ----
    if (c->state == ST_ORIGIN_READING) {
        // Deliberately no EPOLLHUP check before this read. The origin closes
        // as soon as it has answered (HTTP/1.0), so EPOLLIN and EPOLLHUP
        // routinely arrive together — bailing out on HUP would throw away the
        // response we just asked for. read() still returns buffered data after
        // a hangup, and then 0. Read first, decide after.
        for (;;) {
            size_t space = sizeof(c->buf) - 1 - c->len;
            if (space == 0) {
                // Response bigger than our buffer. Never cache a truncated
                // body — a poisoned entry would serve the truncation to every
                // later hit.
                LOGF("TRUNC %s\n", c->key);
                fail_with(epfd, c, RESP_502, sizeof(RESP_502) - 1, cache);
                return;
            }

            ssize_t n = read(c->origin_fd, c->buf + c->len, space);

            if (n > 0) { c->len += (size_t)n; continue; }
            if (n == 0) { origin_finish(epfd, c, cache); return; }   // EOF
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                // More coming later. If the peer also hung up we'd have got 0,
                // not EAGAIN, so this really does mean "wait".
                return;
            }
            fail_with(epfd, c, RESP_502, sizeof(RESP_502) - 1, cache); // ECONNRESET, ...
            return;
        }
    }

    // Any other state means an event on a socket we thought was idle.
    if (e & (EPOLLHUP | EPOLLERR)) {
        fail_with(epfd, c, RESP_502, sizeof(RESP_502) - 1, cache);
    }
}

// ------------------------------------------------------------
//  Client socket events
// ------------------------------------------------------------
// MAY FREE c. Caller must return immediately.
static void on_client_event(int epfd, connection *c, uint32_t e, lru_cache *cache) {
    if (e & (EPOLLHUP | EPOLLERR)) {
        // Client gave up. Drop the in-flight origin fetch with it — nobody is
        // left to receive the answer.
        conn_close(epfd, c);
        return;
    }
    if ((e & EPOLLIN) && c->state == ST_READING) {
        on_readable(epfd, c, cache);
    } else if ((e & EPOLLOUT) && c->state == ST_WRITING) {
        on_writable(epfd, c, cache);
    }
}

// ------------------------------------------------------------
//  main
// ------------------------------------------------------------
static int resolve_origin(void) {
    struct addrinfo hints, *res;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family   = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    int rc = getaddrinfo(ORIGIN_HOST, ORIGIN_PORT, &hints, &res);
    if (rc != 0) {
        fprintf(stderr, "getaddrinfo(%s:%s): %s\n",
                ORIGIN_HOST, ORIGIN_PORT, gai_strerror(rc));
        return -1;
    }
    memcpy(&origin_addr, res->ai_addr, sizeof(origin_addr));
    freeaddrinfo(res);
    return 0;
}

int main(void) {
    lru_cache *cache = cache_create(1024, 100);
    if (!cache) { fprintf(stderr, "cache create failed\n"); exit(1); }

    if (resolve_origin() < 0) exit(1);

    int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0) { perror("socket"); exit(1); }
    int opt = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr;
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(PORT);
    if (bind(listen_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind"); exit(1);
    }
    if (listen(listen_fd, 128) < 0) { perror("listen"); exit(1); }
    set_nonblocking(listen_fd);

    int epfd = epoll_create1(0);
    if (epfd < 0) { perror("epoll_create1"); exit(1); }

    epoll_add(epfd, listen_fd, EPOLLIN, NULL);   // NULL = "this is the listener"

    printf("epoll proxy on port %d -> %s:%s (async origin fetch, client keep-alive)\n",
           PORT, ORIGIN_HOST, ORIGIN_PORT);
    fflush(stdout);

    struct epoll_event events[MAX_EVENTS];

    // ---- THE EVENT LOOP ----
    while (1) {
        int n = epoll_wait(epfd, events, MAX_EVENTS, -1);
        if (n < 0) {
            if (errno == EINTR) continue;
            perror("epoll_wait");
            break;
        }

        for (int i = 0; i < n; i++) {
            void *ptr = events[i].data.ptr;

            if (ptr == NULL) {
                // Listener ready — drain the whole backlog.
                for (;;) {
                    int cfd = accept(listen_fd, NULL, NULL);
                    if (cfd < 0) {
                        if (errno == EINTR) continue;
                        break;                       // EAGAIN: backlog empty
                    }
                    if (set_nonblocking(cfd) < 0) { close(cfd); continue; }

                    connection *nc = calloc(1, sizeof(connection));
                    if (!nc) { close(cfd); continue; }
                    nc->fd        = cfd;
                    nc->origin_fd = -1;
                    nc->state     = ST_READING;

                    nc->client_tag.conn = nc;  nc->client_tag.is_origin = 0;
                    nc->origin_tag.conn = nc;  nc->origin_tag.is_origin = 1;

                    epoll_add(epfd, cfd, EPOLLIN, &nc->client_tag);
                }
                continue;
            }

            ev_tag *tag = (ev_tag *)ptr;
            connection *c = tag->conn;
            uint32_t e = events[i].events;

            if (tag->is_origin) {
                on_origin_event(epfd, c, e, cache);
            } else {
                on_client_event(epfd, c, e, cache);
            }
            // c may be freed at this point — never touch it after dispatch.
        }
    }

    close(listen_fd);
    close(epfd);
    cache_free(cache);
    return 0;
}

// ============================================================
//  Known gaps (deliberate, not oversights)
//
//  1. No timeouts. A client that connects and never sends, or one that goes
//     idle on a kept-alive connection, holds its connection object forever.
//     SO_RCVTIMEO does not apply here — nothing blocks. The fix is a deadline
//     per connection plus a finite epoll_wait timeout and a periodic sweep.
//     This matters MORE now than before keep-alive: idle connections are the
//     normal state of a kept-alive server.
//
//  2. No origin-side keep-alive. Every miss opens a fresh origin connection,
//     unlike server.c / server_threaded.c / server_pool.c, which now pool
//     theirs. Adding it here means replacing the read-until-EOF in
//     ST_ORIGIN_READING with Content-Length framing, keeping a free list of
//     idle origin sockets, and handling the case where a pooled socket turns
//     out to be dead by restarting the fetch. Until then, MISS numbers are not
//     comparable across versions; HIT numbers are.
// ============================================================