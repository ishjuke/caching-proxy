#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>        // read, write, close
#include <arpa/inet.h>     // sockaddr_in, htons, etc.
#include <netdb.h>         // getaddrinfo

// ============================================================
//  Cache implementation (Weeks 1-2)
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
//  Server / reverse proxy (Week 3)
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

// Fetch `path` from the origin server. Writes the origin's full raw HTTP
// response (status line + headers + body) into response_out.
// Returns the number of bytes read, or -1 on error.
ssize_t fetch_from_origin(const char *path, char *response_out, size_t out_size) {
    // --- 1. DNS resolution: hostname + port -> usable address(es) ---
    struct addrinfo hints, *res;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family   = AF_INET;       // IPv4
    hints.ai_socktype = SOCK_STREAM;   // TCP

    if (getaddrinfo(ORIGIN_HOST, ORIGIN_PORT, &hints, &res) != 0) {
        perror("getaddrinfo");
        return -1;
    }

    // --- 2. create a socket and connect to the origin (we're the client now) ---
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

    // --- 3. build and send an HTTP request to the origin ---
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

    // --- 4. read the origin's FULL response ---
    // HTTP/1.0 => origin closes the connection when done => read() returns 0
    // (EOF) once the whole response has arrived. Loop until EOF, error, or the
    // buffer is nearly full. Cap at out_size-1 so the caller can null-terminate
    // at response_out[total] without overflowing.
    ssize_t total = 0;
    while ((size_t)total < out_size - 1) {
        ssize_t r = read(origin_fd, response_out + total, out_size - 1 - (size_t)total);
        if (r < 0) { close(origin_fd); return -1; }   // error
        if (r == 0) break;                            // EOF — origin done
        total += r;
    }

    close(origin_fd);
    return total;
}

// write-all helper: write() may accept fewer bytes than asked under load.
static void write_all(int fd, const char *buf, size_t len) {
    size_t total = 0;
    while (total < len) {
        ssize_t w = write(fd, buf + total, len - total);
        if (w <= 0) break;   // error or client closed
        total += w;
    }
}

int main(void) {
    lru_cache *cache = cache_create(1024, 100);

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

    if (listen(server_fd, 10) < 0) { perror("listen"); exit(1); }
    printf("Proxy listening on port %d, forwarding to %s:%s\n",
           PORT, ORIGIN_HOST, ORIGIN_PORT);

    while (1) {
        int client_fd = accept(server_fd, NULL, NULL);
        if (client_fd < 0) { perror("accept"); continue; }

        char buffer[BUFFER_SIZE];
        ssize_t n = read(client_fd, buffer, BUFFER_SIZE - 1);
        if (n <= 0) { close(client_fd); continue; }
        buffer[n] = '\0';

        // ---- request handling ----
        char key[2048];
        parse_path(buffer, key, sizeof(key));

        // Both branches produce a *complete raw HTTP response* to relay.
        char origin_response[BUFFER_SIZE * 4];   // scratch for a miss fetch
        char *raw_response;
        size_t raw_len;

        char *cached = cache_get(cache, key);
        if (cached) {
            // HIT: cached value is already a complete raw HTTP response
            raw_response = cached;
            raw_len = strlen(cached);
            printf("HIT  %s\n", key);
        } else {
            // MISS: fetch the full response from the origin, cache it, relay it
            ssize_t olen = fetch_from_origin(key, origin_response, sizeof(origin_response));
            if (olen > 0) {
                origin_response[olen] = '\0';
                cache_set(cache, key, origin_response);
                raw_response = origin_response;
                raw_len = (size_t)olen;
            } else {
                // origin unreachable — return a gateway error
                raw_response =
                    "HTTP/1.0 502 Bad Gateway\r\n"
                    "Content-Type: text/plain\r\n"
                    "Content-Length: 16\r\n"
                    "\r\n"
                    "502 Bad Gateway\n";
                raw_len = strlen(raw_response);
            }
            printf("MISS %s\n", key);
        }

        // hit and miss converge here: relay the complete response verbatim
        write_all(client_fd, raw_response, raw_len);

        close(client_fd);
    }

    cache_free(cache);
    close(server_fd);
    return 0;
}