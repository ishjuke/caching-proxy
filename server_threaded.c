#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>        // read, write, close
#include <arpa/inet.h>     // sockaddr_in, htons, etc.
#include <netdb.h>         // getaddrinfo
#include <pthread.h>       // threads + mutex

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
//  Reverse proxy (Week 3) + threading (Week 4 #1)
// ============================================================

#define PORT 8080
#define BUFFER_SIZE 8192

#define ORIGIN_HOST "localhost"
#define ORIGIN_PORT "9000"

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

// Per-connection context handed to each worker thread. We heap-allocate one
// per connection because a stack variable in the accept loop would be
// overwritten by the next accept() before the thread reads it.
typedef struct {
    int client_fd;
    lru_cache *cache;
    pthread_mutex_t *lock;
} client_ctx;

// All the per-request work, run by one thread, fully independent of others
// except where it touches the shared cache (guarded by the mutex).
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
        printf("HIT  %s\n", key);
    } else {
        // MISS: fetch from the origin WITHOUT holding the lock. A network
        // round-trip is slow; holding the mutex across it would serialize every
        // thread on the cache and erase the concurrency we just added.
        ssize_t olen = fetch_from_origin(key, send_buf, sizeof(send_buf));
        if (olen > 0) {
            send_buf[olen] = '\0';     // null-terminate for cache_set's strdup
            send_len = (size_t)olen;

            // re-acquire the lock only to insert — a short critical section
            pthread_mutex_lock(lock);
            cache_set(cache, key, send_buf);
            pthread_mutex_unlock(lock);

            have_response = 1;
        }
        printf("MISS %s\n", key);
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

void *client_thread(void *arg) {
    client_ctx *ctx = (client_ctx *)arg;
    handle_client(ctx->client_fd, ctx->cache, ctx->lock);
    free(ctx);        // this thread owns the ctx; free it when done
    return NULL;
}

int main(void) {
    lru_cache *cache = cache_create(1024, 100);

    pthread_mutex_t cache_lock;
    pthread_mutex_init(&cache_lock, NULL);

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
    printf("Threaded proxy listening on port %d, forwarding to %s:%s\n",
           PORT, ORIGIN_HOST, ORIGIN_PORT);

    while (1) {
        int client_fd = accept(server_fd, NULL, NULL);
        if (client_fd < 0) { perror("accept"); continue; }

        // hand this connection to its own thread, then loop back to accept()
        client_ctx *ctx = malloc(sizeof(client_ctx));
        if (!ctx) { close(client_fd); continue; }
        ctx->client_fd = client_fd;
        ctx->cache     = cache;
        ctx->lock      = &cache_lock;

        pthread_t tid;
        if (pthread_create(&tid, NULL, client_thread, ctx) != 0) {
            perror("pthread_create");
            close(client_fd);
            free(ctx);
            continue;
        }
        pthread_detach(tid);   // we never join; let it clean itself up
    }

    pthread_mutex_destroy(&cache_lock);
    cache_free(cache);
    close(server_fd);
    return 0;
}