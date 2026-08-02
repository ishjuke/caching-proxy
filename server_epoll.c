// ============================================================
//  server_epoll.c — Phase 4b: single-threaded epoll event loop
//                   with a NON-BLOCKING origin fetch.
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
#define ORIGIN_PORT  "9000"

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

static void on_writable(int epfd, connection *c);

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
    if (rlen > sizeof(c->buf)) rlen = sizeof(c->buf);
    memmove(c->buf, resp, rlen);      // memmove: resp may alias c->buf
    c->len = rlen;
    flip_to_writing(epfd, c);
}

static const char RESP_502[] =
    "HTTP/1.0 502 Bad Gateway\r\n"
    "Content-Type: text/plain\r\n"
    "Content-Length: 16\r\n"
    "Connection: close\r\n"
    "\r\n"
    "502 Bad Gateway\n";

static const char RESP_400[] =
    "HTTP/1.0 400 Bad Request\r\n"
    "Content-Type: text/plain\r\n"
    "Content-Length: 16\r\n"
    "Connection: close\r\n"
    "\r\n"
    "400 Bad Request\n";

// Abandon the origin and send an error to the client.
// MAY FREE c (via on_writable) — every caller must `return` straight after.
static void fail_with(int epfd, connection *c, const char *resp, size_t rlen) {
    origin_close(epfd, c);
    begin_response(epfd, c, resp, rlen);
    on_writable(epfd, c);
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
static void start_origin_fetch(int epfd, connection *c) {
    int ofd = socket(AF_INET, SOCK_STREAM, 0);
    if (ofd < 0) { fail_with(epfd, c, RESP_502, sizeof(RESP_502) - 1); return; }

    if (set_nonblocking(ofd) < 0) {
        close(ofd);
        fail_with(epfd, c, RESP_502, sizeof(RESP_502) - 1);
        return;
    }

    int r = connect(ofd, (struct sockaddr *)&origin_addr, sizeof(origin_addr));
    if (r < 0 && errno != EINPROGRESS && errno != EINTR) {
        // EINTR on a non-blocking connect also means "in progress" — don't
        // retry it, that would return EALREADY.
        close(ofd);
        fail_with(epfd, c, RESP_502, sizeof(RESP_502) - 1);
        return;
    }

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
            conn_close(epfd, c);          // client left before finishing
            return;
        } else if (errno != EAGAIN && errno != EWOULDBLOCK) {
            conn_close(epfd, c);
            return;
        }
        // EAGAIN: not an error. Fall through — what we already buffered may
        // well be a complete request line.
    }

    c->buf[c->len] = '\0';
    if (!strstr(c->buf, "\r\n")) {
        if (c->len >= sizeof(c->buf) - 1) {
            fail_with(epfd, c, RESP_400, sizeof(RESP_400) - 1);   // never terminated
            return;
        }
        return;                            // partial request — wait for more
    }

    // Parse the path BEFORE anything overwrites buf.
    parse_path(c->buf, c->key, sizeof(c->key));

    char *cached = cache_get(cache, c->key);
    if (cached) {
        // Copy out immediately. `cached` points into the cache, and this
        // connection lives across many loop iterations — another connection's
        // cache_set (or the eviction it triggers) can free that memory before
        // we finish writing. Same rule as the mutex version, different reason.
        LOGF("HIT  %s\n", c->key);
        begin_response(epfd, c, cached, strlen(cached));
        on_writable(epfd, c);              // usually writable already
        return;
    }

    LOGF("MISS %s\n", c->key);
    start_origin_fetch(epfd, c);
    // NOTE: no on_writable here. The client has nothing to receive yet — it
    // now waits, tracked entirely by its state, while the loop serves others.
}

// ------------------------------------------------------------
//  Client socket WRITABLE (state ST_WRITING)
// ------------------------------------------------------------
// MAY FREE c. Caller must return immediately.
static void on_writable(int epfd, connection *c) {
    while (c->off < c->len) {
        ssize_t n = write(c->fd, c->buf + c->off, c->len - c->off);

        if (n > 0) {
            c->off += (size_t)n;           // partial write: resume from here
            continue;
        }
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            return;                        // send buffer full; wait for EPOLLOUT
        }
        conn_close(epfd, c);               // EPIPE, ECONNRESET, ...
        return;
    }

    conn_close(epfd, c);                   // fully sent
}

// ------------------------------------------------------------
//  Origin response complete (EOF from origin)
// ------------------------------------------------------------
// MAY FREE c. Caller must return immediately.
static void origin_finish(int epfd, connection *c, lru_cache *cache) {
    origin_close(epfd, c);

    if (c->len == 0) {                     // origin closed without answering
        fail_with(epfd, c, RESP_502, sizeof(RESP_502) - 1);
        return;
    }

    c->buf[c->len] = '\0';                 // cache_set's strdup needs the NUL
    cache_set(cache, c->key, c->buf);

    // buf already holds the response, in place — nothing to copy.
    flip_to_writing(epfd, c);
    on_writable(epfd, c);
}

// ------------------------------------------------------------
//  Origin socket events — the dual-FD half of the state machine
// ------------------------------------------------------------
// MAY FREE c. Caller must return immediately.
static void on_origin_event(int epfd, connection *c, uint32_t e, lru_cache *cache) {

    // ---- step 2: the connect finished (or failed) ----
    if (c->state == ST_ORIGIN_CONNECTING) {
        if (e & EPOLLERR) {
            fail_with(epfd, c, RESP_502, sizeof(RESP_502) - 1);
            return;
        }

        // Writable means the handshake COMPLETED, not that it SUCCEEDED. A
        // refused connection also wakes us up writable. SO_ERROR is where the
        // kernel left the verdict; 0 means genuinely connected.
        int soerr = 0;
        socklen_t slen = sizeof(soerr);
        if (getsockopt(c->origin_fd, SOL_SOCKET, SO_ERROR, &soerr, &slen) < 0 || soerr != 0) {
            errno = soerr;
            fail_with(epfd, c, RESP_502, sizeof(RESP_502) - 1);
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
            fail_with(epfd, c, RESP_502, sizeof(RESP_502) - 1);
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
                fail_with(epfd, c, RESP_502, sizeof(RESP_502) - 1);
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
            fail_with(epfd, c, RESP_502, sizeof(RESP_502) - 1);      // ECONNRESET, ...
            return;
        }
    }

    // Any other state means an event on a socket we thought was idle.
    if (e & (EPOLLHUP | EPOLLERR)) {
        fail_with(epfd, c, RESP_502, sizeof(RESP_502) - 1);
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
        on_writable(epfd, c);
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

    printf("epoll proxy on port %d -> %s:%s (phase 4b: async origin fetch)\n",
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