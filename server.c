#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>        // read, write, close
#include <arpa/inet.h>     // sockaddr_in, htons, etc.

// ============================================================
//  Cache implementation (Weeks 1-2) — old main() removed
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
//  Server (Week 3)
// ============================================================

#define PORT 8080
#define BUFFER_SIZE 8192

// Extract the request path from an HTTP request line like:
//   "GET /index.html HTTP/1.1"
// We want the middle token ("/index.html") as the cache key.
void parse_path(const char *request, char *out, size_t out_size) {
    // Build a bounded format string so a long path can't overflow `out`.
    // "%*s" skips the method (GET); "%<N>s" reads the path capped at N chars.
    char fmt[32];
    snprintf(fmt, sizeof(fmt), "%%*s %%%zus", out_size - 1);

    out[0] = '\0';
    if (sscanf(request, fmt, out) != 1) {
        // malformed request line — fall back to root
        strncpy(out, "/", out_size);
        out[out_size - 1] = '\0';
    }
}

int main(void) {
    // 1. create the cache
    lru_cache *cache = cache_create(1024, 100);

    // 2. create the listening socket
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) { perror("socket"); exit(1); }

    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    // 3. bind
    struct sockaddr_in addr;
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(PORT);
    if (bind(server_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind"); exit(1);
    }

    // 4. listen
    if (listen(server_fd, 10) < 0) { perror("listen"); exit(1); }
    printf("Listening on port %d...\n", PORT);

    // 5. accept loop — one client at a time
    while (1) {
        int client_fd = accept(server_fd, NULL, NULL);
        if (client_fd < 0) { perror("accept"); continue; }

        char buffer[BUFFER_SIZE];
        ssize_t n = read(client_fd, buffer, BUFFER_SIZE - 1);
        if (n <= 0) { close(client_fd); continue; }
        buffer[n] = '\0';

        // ---- request handling ----

        // a) parse the path into `key`
        char key[2048];
        parse_path(buffer, key, sizeof(key));

        // b) look it up; build the body from cache on HIT, or make + cache on MISS
        char body[BUFFER_SIZE];
        char *cached = cache_get(cache, key);
        if (cached) {
            snprintf(body, sizeof(body), "%s", cached);
            printf("HIT  %s\n", key);
        } else {
            snprintf(body, sizeof(body), "Hello! You requested: %s\n", key);
            cache_set(cache, key, body);
            printf("MISS %s\n", key);
        }

        // c) build the full HTTP response — exact format, Content-Length must
        //    equal the body's real byte count or curl hangs.
        char response[BUFFER_SIZE * 2];
        int len = snprintf(response, sizeof(response),
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: text/plain\r\n"
            "Content-Length: %zu\r\n"
            "\r\n"
            "%s",
            strlen(body), body);

        // write-all: write() may send fewer bytes than asked (partial write),
        // especially under load. Loop until the whole response is out.
        ssize_t total = 0;
        while (total < len) {
            ssize_t w = write(client_fd, response + total, len - total);
            if (w <= 0) break;   // error or client closed the connection
            total += w;
        }

        close(client_fd);
    }

    cache_free(cache);
    close(server_fd);
    return 0;
}