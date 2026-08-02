#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>        // read, write, close
#include <arpa/inet.h>     // sockaddr_in, htons, etc.
#include <netdb.h>         // getaddrinfo
#include <pthread.h>       // threads + mutex + condvar

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
//  Reverse proxy (Week 3) + thread pool (Week 4 #2)
// ============================================================

#define PORT 8080
#define BUFFER_SIZE 8192

#define ORIGIN_HOST "localhost"
#define ORIGIN_PORT "9000"

#define THREAD_POOL_SIZE  16     // number of worker threads
#define QUEUE_CAPACITY   256    // max pending connections waiting for a worker

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
//  HTTP / origin plumbing — unchanged
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

ssize_t fetch_from_origin(const char *path, char *response_out, size_t out_size) {
    struct addrinfo hints, *res;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family   = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    if (getaddrinfo(ORIGIN_HOST, ORIGIN_PORT, &hints, &res) != 0) {
        perror("getaddrinfo");
        return -1;
    }

    int origin_fd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (origin_fd < 0) {
        perror("socket");
        freeaddrinfo(res);
        return -1;
    }
    if (connect(origin_fd, res->ai_addr, res->ai_addrlen) < 0) {
        perror("connect");
        close(origin_fd);
        freeaddrinfo(res);
        return -1;
    }
    freeaddrinfo(res);

    char request[2048];
    int req_len = snprintf(request, sizeof(request),
        "GET %s HTTP/1.0\r\n"
        "Host: %s\r\n"
        "\r\n",
        path, ORIGIN_HOST);

    ssize_t sent = 0;
    while (sent < req_len) {
        ssize_t w = write(origin_fd, request + sent, req_len - sent);
        if (w <= 0) { close(origin_fd); return -1; }
        sent += w;
    }

    ssize_t total = 0;
    while ((size_t)total < out_size - 1) {
        ssize_t r = read(origin_fd, response_out + total, out_size - 1 - (size_t)total);
        if (r < 0) { close(origin_fd); return -1; }
        if (r == 0) break;   // EOF — origin done
        total += r;
    }

    close(origin_fd);
    return total;
}

static void write_all(int fd, const char *buf, size_t len) {
    size_t total = 0;
    while (total < len) {
        ssize_t w = write(fd, buf + total, len - total);
        if (w <= 0) break;
        total += w;
    }
}

// All the per-request work. Identical to the thread-per-connection version —
// the pool changes how this gets CALLED, not what it does.
void handle_client(int client_fd, lru_cache *cache, pthread_mutex_t *lock) {
    char buffer[BUFFER_SIZE];
    ssize_t n = read(client_fd, buffer, BUFFER_SIZE - 1);
    if (n <= 0) { close(client_fd); return; }
    buffer[n] = '\0';

    char key[2048];
    parse_path(buffer, key, sizeof(key));

    char send_buf[BUFFER_SIZE * 4];   // this thread's private copy to send
    size_t send_len = 0;
    int have_response = 0;

    // --- cache lookup: critical section ---
    pthread_mutex_lock(lock);
    char *cached = cache_get(cache, key);
    if (cached) {
        // COPY OUT WHILE STILL HOLDING THE LOCK. The moment we unlock, another
        // thread's cache_set (update branch frees the old value) or an eviction
        // could free this exact memory — sending from `cached` after unlock is
        // a use-after-free. So we snapshot it into our private buffer first.
        send_len = strlen(cached);
        if (send_len >= sizeof(send_buf)) send_len = sizeof(send_buf) - 1;
        memcpy(send_buf, cached, send_len);
        have_response = 1;
    }
    pthread_mutex_unlock(lock);

    if (have_response) {
        //printf("HIT  %s\n", key);
    } else {
        // MISS: fetch from the origin WITHOUT holding the lock.
        ssize_t olen = fetch_from_origin(key, send_buf, sizeof(send_buf));
        if (olen > 0) {
            send_buf[olen] = '\0';     // null-terminate for cache_set's strdup
            send_len = (size_t)olen;

            pthread_mutex_lock(lock);
            cache_set(cache, key, send_buf);
            pthread_mutex_unlock(lock);

            have_response = 1;
        }
        //printf("MISS %s\n", key);
    }

    if (have_response) {
        write_all(client_fd, send_buf, send_len);
    } else {
        const char *bad =
            "HTTP/1.0 502 Bad Gateway\r\n"
            "Content-Type: text/plain\r\n"
            "Content-Length: 16\r\n"
            "\r\n"
            "502 Bad Gateway\n";
        write_all(client_fd, bad, strlen(bad));
    }

    close(client_fd);
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
    }

    return NULL;
}

// ------------------------------------------------------------
//  main — spawn the pool once, then just accept and enqueue
// ------------------------------------------------------------

int main(void) {
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

    printf("Thread-pool proxy on port %d (%d workers) -> %s:%s\n",
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