#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>       // strncasecmp
#include <unistd.h>        // read, write, close
#include <arpa/inet.h>     // sockaddr_in, htons, etc.
#include <netdb.h>         // getaddrinfo
#include <errno.h>
#include <sys/time.h>      // struct timeval (SO_RCVTIMEO)
#include <pthread.h>       // threads + mutex + condvar
#include <signal.h>
static _Atomic long brk_read_eof = 0;    // line 578: read returned <= 0
static _Atomic long brk_write_fail = 0;  // line 637: write to client failed
static _Atomic long brk_502 = 0;         // line 647: gateway error, hung up
static _Atomic long brk_done = 0;        // line 651: keep-alive done / client wants close

static void dump_break_stats(int sig) {
    (void)sig;
    fprintf(stderr, "\n--- break tallies ---\n");
    fprintf(stderr, "read_eof:   %ld\n", brk_read_eof);
    fprintf(stderr, "write_fail: %ld\n", brk_write_fail);
    fprintf(stderr, "502:        %ld\n", brk_502);
    fprintf(stderr, "done/close: %ld\n", brk_done);
    _exit(0);
}
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
//  Reverse proxy (Week 3) + thread pool (Week 4 #2) + keep-alive
// ============================================================

#define PORT 8080
#define BUFFER_SIZE 8192

#define ORIGIN_HOST "localhost"
#define ORIGIN_PORT "80"          // nginx

#define THREAD_POOL_SIZE  16     // number of worker threads
#define QUEUE_CAPACITY   256    // max pending connections waiting for a worker

// Idle timeout on a kept-alive CLIENT connection.
//
// This constant costs more here than in any other version. A worker parked on
// an idle-but-open connection is a worker that cannot serve anyone else, so
// every idle second is a worker-second burned. With THREAD_POOL_SIZE workers
// you can only serve THREAD_POOL_SIZE *connections* at once — not requests.
// See the note above handle_client().
#define IDLE_TIMEOUT_SEC 15

// Timeout on the ORIGIN socket. Mandatory now that we ask nginx for keep-alive:
// nginx no longer closes to signal "done", so a framing bug would otherwise
// block a worker until nginx's keepalive_timeout (75s) expires.
#define ORIGIN_TIMEOUT_SEC 5

// Kept identical to server.c and server_threaded.c so the version comparison
// stays apples-to-apples. It also bounds how long one client can hold a worker.
#define MAX_KEEPALIVE_REQUESTS 100

// ------------------------------------------------------------
//  Thread-safe bounded job queue
// ------------------------------------------------------------
//
// Circular buffer of pending client FDs. head is where we pop, tail is where
// we push, count tracks size. FDs are stored BY VALUE (int, not int*) so no
// worker can ever race on a reused slot the way it would with a pointer to
// the accept loop's stack local.

typedef struct {
    int fds[QUEUE_CAPACITY];
    int head;
    int tail;
    int count;
    pthread_mutex_t lock;
    pthread_cond_t  not_empty;   // workers wait on this when the queue is empty
} job_queue;

void queue_init(job_queue *q) {
    q->head = 0;
    q->tail = 0;
    q->count = 0;
    pthread_mutex_init(&q->lock, NULL);
    pthread_cond_init(&q->not_empty, NULL);
}

// Push a client FD onto the queue. Called by the accept loop (the producer).
// On overflow we drop the connection rather than block: a wedged accept loop
// stops draining the kernel's backlog and is worse than a refused client.
void queue_push(job_queue *q, int client_fd) {
    pthread_mutex_lock(&q->lock);

    if (q->count == QUEUE_CAPACITY) {
        pthread_mutex_unlock(&q->lock);
        close(client_fd);   // syscall — deliberately outside the lock
        return;
    }

    q->fds[q->tail] = client_fd;
    q->tail = (q->tail + 1) % QUEUE_CAPACITY;
    q->count++;

    // One push == one job, so signal (wake one) rather than broadcast.
    pthread_cond_signal(&q->not_empty);
    pthread_mutex_unlock(&q->lock);
}

// Pop a client FD. Called by worker threads (the consumers).
// Blocks (sleeps, does not spin) while the queue is empty.
int queue_pop(job_queue *q) {
    pthread_mutex_lock(&q->lock);

    // WHILE, not if: a worker can wake to find the queue already emptied by
    // another worker, or wake spuriously. cond_wait atomically releases the
    // lock while sleeping and re-acquires it on wake.
    while (q->count == 0) {
        pthread_cond_wait(&q->not_empty, &q->lock);
    }

    // Copy the fd into a local BEFORE unlocking — this local is what survives
    // the unlock and gets returned.
    int fd = q->fds[q->head];
    q->head = (q->head + 1) % QUEUE_CAPACITY;
    q->count--;

    pthread_mutex_unlock(&q->lock);
    return fd;
}

// ------------------------------------------------------------
//  HTTP plumbing
// ------------------------------------------------------------

void parse_path(const char *request, char *out, size_t out_size) {
    char fmt[32];
    snprintf(fmt, sizeof(fmt), "%%*s %%%zus", out_size - 1);

    out[0] = '\0';
    if (sscanf(request, fmt, out) != 1) {
        strncpy(out, "/", out_size);
        out[out_size - 1] = '\0';
    }
}

// write-all helper. Returns 0 on success, -1 if the peer went away mid-write —
// which the keep-alive loop needs so it can stop instead of reading again.
static int write_all(int fd, const char *buf, size_t len) {
    size_t total = 0;
    while (total < len) {
        ssize_t w = write(fd, buf + total, len - total);
        if (w <= 0) return -1;
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
//  Origin connection — persistent, PER WORKER
// ------------------------------------------------------------
// __thread gives every worker its own copy. Thread A's origin socket and
// thread B's are different fds and neither can see the other's, so the reuse
// needs no mutex — the isolation is what removes the race, not a lock.
//
// This is where __thread finally pays off. The workers here are IMMORTAL:
// worker_loop never returns, so g_origin_fd persists for the entire life of
// the process and is reused across every client that worker ever handles.
// THREAD_POOL_SIZE workers => THREAD_POOL_SIZE long-lived origin connections,
// total, no matter how many clients arrive.
//
// Consequence: worker_loop must NOT call origin_drop(). In server_threaded.c
// the drop was mandatory because threads die per connection and would orphan
// the fd. Here a drop would destroy the very reuse this exists for. The fd
// count is bounded by worker count, not by connection count, so nothing leaks.

static __thread int g_origin_fd = -1;

static void origin_drop(void) {
    if (g_origin_fd >= 0) { close(g_origin_fd); g_origin_fd = -1; }
}

static int origin_connect(void) {
    struct addrinfo hints, *res;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family   = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

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

// One attempt on this worker's current origin socket.
//   >0  bytes of a complete response in `out`
//   -1  socket looks stale and nothing was received — safe to retry fresh
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

    // We can no longer read until EOF: nginx holds the connection open, so
    // there is no EOF. Content-Length is what tells us where the body ends.
    size_t total = 0;
    char *hdr_end = NULL;

    while (!hdr_end) {
        if (total >= out_size - 1) return -2;         // headers bigger than our buffer
        ssize_t r = read(g_origin_fd, out + total, out_size - 1 - total);
        if (r <= 0) {
            // Nothing received yet => almost certainly a pooled socket nginx
            // had already timed out. This is the COMMON case in the pool: a
            // worker's origin socket can sit idle for minutes between misses.
            return total == 0 ? -1 : -2;
        }
        total += (size_t)r;
        out[total] = '\0';
        hdr_end = strstr(out, "\r\n\r\n");
    }
    size_t header_len = (size_t)(hdr_end - out) + 4;  // includes the blank line

    const char *cl = find_ci(out, header_len, "\r\ncontent-length:");
    if (!cl) return -2;    // chunked or something we don't handle: refuse, don't guess

    long long body_len = strtoll(cl + strlen("\r\ncontent-length:"), NULL, 10);
    if (body_len < 0) return -2;

    size_t need = header_len + (size_t)body_len;
    if (need > out_size - 1) return -2;               // too big to buffer/cache

    // Read exactly the rest of the body and no further. Reading past `need`
    // would swallow the front of the NEXT response on this reused connection
    // and desynchronise every request this worker makes afterwards.
    while (total < need) {
        ssize_t r = read(g_origin_fd, out + total, need - total);
        if (r <= 0) return -2;
        total += (size_t)r;
    }
    out[total] = '\0';

    if (find_ci(out, header_len, "connection: close")) origin_drop();

    return (ssize_t)total;
}

// Fetch `path` from the origin, reusing this worker's connection when alive.
ssize_t fetch_from_origin(const char *path, char *response_out, size_t out_size) {
    for (int attempt = 0; attempt < 2; attempt++) {
        if (g_origin_fd < 0) {
            g_origin_fd = origin_connect();
            if (g_origin_fd < 0) return -1;
        }

        ssize_t r = origin_fetch_once(path, response_out, out_size);
        if (r > 0) return r;

        origin_drop();
        if (r == -2) return -1;   // real failure — retrying won't help
        // r == -1: stale pooled socket. Retry once with a fresh connection.
        // Safe: nothing of the response arrived, and GET is idempotent.
        // Load-bearing here — nginx closes idle keep-alive connections after
        // keepalive_timeout (75s) and after keepalive_requests (1000).
    }
    return -1;
}

// ------------------------------------------------------------
//  Response normalization
// ------------------------------------------------------------
// Connection (and friends) are HOP-BY-HOP headers: they describe one TCP link,
// not the end-to-end message. Relaying nginx's Connection header to the client
// is what told wrk to hang up — and caching it would serve that close directive
// forever. So: strip them, and state our own.

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
// Thread-safe: touches only the caller's buffer and its own stack.
static ssize_t normalize_response(char *resp, size_t len, size_t cap) {
    char *hdr_end = strstr(resp, "\r\n\r\n");
    if (!hdr_end) return -1;

    size_t header_block = (size_t)(hdr_end - resp) + 2;   // through last header's CRLF
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

// A connection can only be reused if the client can find the end of the
// response without waiting for EOF: Content-Length present, no Connection: close.

// An HTTP/1.0 response with no explicit keep-alive means "closes when done" to
// every client. One-byte edit, since the version tokens are the same length.
static void upgrade_to_http11(char *resp) {
    if (strncmp(resp, "HTTP/1.0", 8) == 0) resp[7] = '1';
}

// Did the CLIENT ask us to close? Its request is the authority on the client
// link, the same way our request is the authority on the origin link.
static int client_requested_close(const char *req, size_t len) {
    const char *end = strstr(req, "\r\n\r\n");
    size_t hlen = end ? (size_t)(end - req) + 2 : len;
    if (find_ci(req, hlen, "\r\nconnection: close")) return 1;
    const char *eol = strstr(req, "\r\n");
    size_t line = eol ? (size_t)(eol - req) : len;
    if (find_ci(req, line, "HTTP/1.0") && !find_ci(req, hlen, "connection: keep-alive")) return 1;
    return 0;
}

// ------------------------------------------------------------
//  Per-connection work, run by one worker
// ------------------------------------------------------------
// IMPORTANT CHANGE IN CHARACTER: before keep-alive, a worker was busy for one
// REQUEST. Now it is busy for one CONNECTION — every request on it, plus up to
// IDLE_TIMEOUT_SEC of the client thinking. The pool's capacity is therefore
// THREAD_POOL_SIZE concurrent *connections*. Offer it more concurrent clients
// than that and the surplus sit in the queue, unserved, however idle the
// workers look. This is the tradeoff keep-alive buys, and the reason the epoll
// version exists.
void handle_client(int client_fd, lru_cache *cache, pthread_mutex_t *lock) {
    struct timeval tv = { .tv_sec = IDLE_TIMEOUT_SEC, .tv_usec = 0 };
    setsockopt(client_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(client_fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    int served = 0;
    while (served < MAX_KEEPALIVE_REQUESTS) {
        char buffer[BUFFER_SIZE];

        // Framing simplification: we assume one read() yields exactly one
        // complete request. True in practice for wrk's small GETs on a LAN,
        // not in general. The epoll version buffers properly.
        ssize_t n = read(client_fd, buffer, BUFFER_SIZE - 1);
        if (n <= 0) {
            // n == 0 : client closed cleanly.
            // n <  0 : error, or EAGAIN/EWOULDBLOCK from SO_RCVTIMEO — the
            //          client went idle. Either way we're done with it, and
            //          this worker goes back to the queue.
            { brk_read_eof++; break; }
        }
        buffer[n] = '\0';

        int wants_close = client_requested_close(buffer, (size_t)n);

        char key[2048];
        parse_path(buffer, key, sizeof(key));

        char send_buf[BUFFER_SIZE * 4];   // this worker's private copy to send
        size_t send_len = 0;
        int have_response = 0;
        int keep_alive = 0;

        // --- cache lookup: critical section ---
        pthread_mutex_lock(lock);
        char *cached = cache_get(cache, key);
        if (cached) {
            // COPY OUT WHILE STILL HOLDING THE LOCK. The moment we unlock,
            // another thread's cache_set (update branch frees the old value) or
            // an eviction could free this exact memory — sending from `cached`
            // after unlock is a use-after-free. So we snapshot it first.
            send_len = strlen(cached);
            if (send_len >= sizeof(send_buf)) send_len = sizeof(send_buf) - 1;
            memcpy(send_buf, cached, send_len);
            send_buf[send_len] = '\0';   // the copy is now used with strstr too
            have_response = 1;
        }
        pthread_mutex_unlock(lock);

        if (have_response) {
            // Runs on our private copy, never on cache memory — no lock needed.
            keep_alive = !wants_close;
            //printf("HIT  %s\n", key);
        } else {
            // MISS: fetch from the origin WITHOUT holding the lock. A network
            // round-trip is slow; holding the mutex across it would serialize
            // every worker on the cache and turn the pool into one thread.
            ssize_t olen = fetch_from_origin(key, send_buf, sizeof(send_buf));
            if (olen > 0) {
                send_buf[olen] = '\0';
                olen = normalize_response(send_buf, (size_t)olen, sizeof(send_buf));
            }

            if (olen > 0) {
                upgrade_to_http11(send_buf);
                send_len = (size_t)olen;

                pthread_mutex_lock(lock);
                cache_set(cache, key, send_buf);   // cache the NORMALIZED form
                pthread_mutex_unlock(lock);

                have_response = 1;
                keep_alive = !wants_close;
            }
            //printf("MISS %s\n", key);
        }

        if (have_response) {
            if (write_all(client_fd, send_buf, send_len) < 0) { brk_write_fail++; break; }
        } else {
            const char *bad =
                "HTTP/1.1 502 Bad Gateway\r\n"
                "Content-Type: text/plain\r\n"
                "Content-Length: 16\r\n"
                "Connection: close\r\n"
                "\r\n"
                "502 Bad Gateway\n";
            write_all(client_fd, bad, strlen(bad));
            { brk_502++; break; }                      // gateway error — hang up
        }

        served++;
        if (!keep_alive || wants_close) { brk_done++; break; }
        // no close here: loop back and read the next request
    }

    close(client_fd);   // closed once, after the client is done with us
}

// ------------------------------------------------------------
//  Worker threads
// ------------------------------------------------------------

// Context shared by ALL workers — read-only for the life of the process, so
// there's no per-request malloc the way client_ctx needed.
typedef struct {
    job_queue *queue;
    lru_cache *cache;
    pthread_mutex_t *cache_lock;
} worker_ctx;

void *worker_loop(void *arg) {
    worker_ctx *w = (worker_ctx *)arg;

    for (;;) {
        int fd = queue_pop(w->queue);   // blocks (sleeps) while idle

        // OUTSIDE the queue lock — queue_pop released it before returning.
        // If this ran inside the critical section every worker would serialize
        // here and the pool would be a single-threaded server with extra steps.
        handle_client(fd, w->cache, w->cache_lock);

        // No cleanup: handle_client closes client_fd on every path.
        //
        // And deliberately NO origin_drop() here. server_threaded.c needs one
        // because its threads die per connection; this loop never exits, so the
        // worker's origin connection is meant to survive into the next client.
        // Dropping it here would silently undo the whole point of __thread.
    }

    return NULL;
}

// ------------------------------------------------------------
//  main — spawn the pool once, then just accept and enqueue
// ------------------------------------------------------------

int main(void) {
    signal(SIGINT, dump_break_stats);
    lru_cache *cache = cache_create(1024, 100);
    if (!cache) { fprintf(stderr, "failed to create cache\n"); exit(1); }

    pthread_mutex_t cache_lock;
    pthread_mutex_init(&cache_lock, NULL);

    job_queue queue;
    queue_init(&queue);

    // Stack-local in main, but main never returns while workers are alive,
    // so the workers' pointer to it stays valid for the process lifetime.
    worker_ctx wctx = { .queue = &queue, .cache = cache, .cache_lock = &cache_lock };

    // --- spin up the fixed pool of workers ONCE ---
    pthread_t workers[THREAD_POOL_SIZE];
    for (int i = 0; i < THREAD_POOL_SIZE; i++) {
        if (pthread_create(&workers[i], NULL, worker_loop, &wctx) != 0) {
            perror("pthread_create");
            exit(1);   // fatal at startup: an undersized pool would silently
                       // invalidate the benchmark
        }
        pthread_detach(workers[i]);
    }

    // --- socket setup ---
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
    if (listen(server_fd, 128) < 0) { perror("listen"); exit(1); }

    printf("Thread-pool proxy on port %d (%d workers) -> %s:%s "
           "(keep-alive: client + origin)\n",
           PORT, THREAD_POOL_SIZE, ORIGIN_HOST, ORIGIN_PORT);

    // --- accept loop: accept -> enqueue -> loop. No thread creation here. ---
    while (1) {
        int client_fd = accept(server_fd, NULL, NULL);
        if (client_fd < 0) { perror("accept"); continue; }
        queue_push(&queue, client_fd);
    }

    // (unreached)
    pthread_mutex_destroy(&cache_lock);
    cache_free(cache);
    close(server_fd);
    return 0;
}