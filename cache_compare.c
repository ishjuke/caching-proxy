// cache_compare.c — head-to-head LRU vs LFU hit-rate comparison.
// Both caches see the identical request stream; we count hits/misses for each.
//
// The interesting question isn't speed, it's HIT RATE under a given access
// pattern. We run two workloads to show the answer depends on the workload:
//   1. static skew  (stable Zipfian popularity)  -> LFU should do well
//   2. drifting skew (popularity moves over time) -> LRU should do well,
//      because LFU clings to items that were popular in the past.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

// ============================================================
//  Shared hash
// ============================================================
unsigned long hash(const char *key, int size) {
    unsigned long h = 5381;
    int c;
    while ((c = *key++)) h = h * 33 + c;
    return h % size;
}

// ============================================================
//  LRU cache (from the project, prefixed lru_)
// ============================================================
typedef struct lru_entry {
    char *key, *value;
    struct lru_entry *hnext;   // hash chain
    struct lru_entry *prev, *next;  // recency list
} lru_entry;

typedef struct {
    lru_entry **buckets;
    int size, count, capacity;
    lru_entry *head, *tail;
} lru_cache;

lru_cache *lru_create(int size, int capacity) {
    lru_cache *c = malloc(sizeof(lru_cache));
    c->buckets = calloc(size, sizeof(lru_entry *));
    c->size = size; c->count = 0; c->capacity = capacity;
    c->head = c->tail = NULL;
    return c;
}

void lru_list_remove(lru_cache *c, lru_entry *e) {
    if (e->prev) e->prev->next = e->next; else c->head = e->next;
    if (e->next) e->next->prev = e->prev; else c->tail = e->prev;
    e->prev = e->next = NULL;
}

void lru_list_push_front(lru_cache *c, lru_entry *e) {
    e->prev = NULL; e->next = c->head;
    if (c->head) c->head->prev = e; else c->tail = e;
    c->head = e;
}

void lru_evict(lru_cache *c) {
    lru_entry *v = c->tail;
    if (!v) return;
    lru_list_remove(c, v);
    unsigned long idx = hash(v->key, c->size);
    lru_entry *cur = c->buckets[idx], *prev = NULL;
    while (cur != v) { prev = cur; cur = cur->hnext; }
    if (prev) prev->hnext = cur->hnext; else c->buckets[idx] = cur->hnext;
    free(v->key); free(v->value); free(v); c->count--;
}

char *lru_get(lru_cache *c, const char *key) {
    unsigned long idx = hash(key, c->size);
    for (lru_entry *e = c->buckets[idx]; e; e = e->hnext) {
        if (strcmp(e->key, key) == 0) {
            lru_list_remove(c, e); lru_list_push_front(c, e);
            return e->value;
        }
    }
    return NULL;
}

void lru_set(lru_cache *c, const char *key, const char *value) {
    unsigned long idx = hash(key, c->size);
    for (lru_entry *e = c->buckets[idx]; e; e = e->hnext) {
        if (strcmp(e->key, key) == 0) {
            free(e->value); e->value = strdup(value);
            lru_list_remove(c, e); lru_list_push_front(c, e);
            return;
        }
    }
    lru_entry *ne = malloc(sizeof(lru_entry));
    ne->key = strdup(key); ne->value = strdup(value);
    ne->hnext = c->buckets[idx]; c->buckets[idx] = ne;
    lru_list_push_front(c, ne);
    c->count++;
    if (c->count > c->capacity) lru_evict(c);
}

void lru_free(lru_cache *c) {
    lru_entry *e = c->head;
    while (e) { lru_entry *n = e->next; free(e->key); free(e->value); free(e); e = n; }
    free(c->buckets); free(c);
}

// ============================================================
//  LFU cache (new). Same hash table for lookup, but no recency
//  list — each entry carries a freq counter, and eviction scans
//  all resident entries for the minimum. The "all-entries" list
//  (prev/next) exists only so we can iterate + O(1)-unlink; it is
//  NOT recency-ordered.
// ============================================================
typedef struct lfu_entry {
    char *key, *value;
    int freq;                        // times accessed
    struct lfu_entry *hnext;         // hash chain
    struct lfu_entry *prev, *next;   // all-entries list (for scanning)
} lfu_entry;

typedef struct {
    lfu_entry **buckets;
    int size, count, capacity;
    lfu_entry *head;                 // head of the all-entries list
} lfu_cache;

lfu_cache *lfu_create(int size, int capacity) {
    lfu_cache *c = malloc(sizeof(lfu_cache));
    c->buckets = calloc(size, sizeof(lfu_entry *));
    c->size = size; c->count = 0; c->capacity = capacity;
    c->head = NULL;
    return c;
}

// unlink from the all-entries list
static void lfu_list_unlink(lfu_cache *c, lfu_entry *e) {
    if (e->prev) e->prev->next = e->next; else c->head = e->next;
    if (e->next) e->next->prev = e->prev;
}

// push onto the front of the all-entries list
static void lfu_list_push_front(lfu_cache *c, lfu_entry *e) {
    e->prev = NULL; e->next = c->head;
    if (c->head) c->head->prev = e;
    c->head = e;
}

// THE ONE GENUINELY NEW PIECE: scan every resident entry, evict the
// minimum-frequency one. Tie-break toward the OLDEST such entry
// (scan head->tail, `<=` keeps the last/closest-to-tail min) so that a
// freshly inserted freq-1 item isn't instantly evicted over an equally
// cold but older one.
void lfu_evict(lfu_cache *c) {
    if (!c->head) return;
    lfu_entry *victim = c->head;
    int minf = victim->freq;
    for (lfu_entry *e = c->head->next; e; e = e->next) {
        if (e->freq <= minf) { minf = e->freq; victim = e; }
    }
    // unlink from all-entries list
    lfu_list_unlink(c, victim);
    // unlink from hash bucket chain (trailing-pointer walk)
    unsigned long idx = hash(victim->key, c->size);
    lfu_entry *cur = c->buckets[idx], *prev = NULL;
    while (cur != victim) { prev = cur; cur = cur->hnext; }
    if (prev) prev->hnext = cur->hnext; else c->buckets[idx] = cur->hnext;
    free(victim->key); free(victim->value); free(victim); c->count--;
}

char *lfu_get(lfu_cache *c, const char *key) {
    unsigned long idx = hash(key, c->size);
    for (lfu_entry *e = c->buckets[idx]; e; e = e->hnext) {
        if (strcmp(e->key, key) == 0) {
            e->freq++;              // the access
            return e->value;
        }
    }
    return NULL;
}

void lfu_set(lfu_cache *c, const char *key, const char *value) {
    unsigned long idx = hash(key, c->size);
    for (lfu_entry *e = c->buckets[idx]; e; e = e->hnext) {
        if (strcmp(e->key, key) == 0) {
            free(e->value); e->value = strdup(value);
            e->freq++;
            return;
        }
    }
    lfu_entry *ne = malloc(sizeof(lfu_entry));
    ne->key = strdup(key); ne->value = strdup(value);
    ne->freq = 1;
    ne->hnext = c->buckets[idx]; c->buckets[idx] = ne;
    lfu_list_push_front(c, ne);
    c->count++;
    if (c->count > c->capacity) lfu_evict(c);
}

void lfu_free(lfu_cache *c) {
    lfu_entry *e = c->head;
    while (e) { lfu_entry *n = e->next; free(e->key); free(e->value); free(e); e = n; }
    free(c->buckets); free(c);
}

// ============================================================
//  Workload generation
// ============================================================
static double rand01(void) {
    return (double)rand() / ((double)RAND_MAX + 1.0);
}

// Generate N requests over K keys, popularity ~ Zipf(s).
// drift_span > 0 shifts the "hot set" across the keyspace over the run,
// simulating popularity that moves over time (news cycle, trending, etc).
int *gen_stream(int N, int K, double s, int drift_span) {
    double *cdf = malloc(K * sizeof(double));
    double sum = 0;
    for (int i = 0; i < K; i++) sum += 1.0 / pow(i + 1, s);
    double acc = 0;
    for (int i = 0; i < K; i++) { acc += (1.0 / pow(i + 1, s)) / sum; cdf[i] = acc; }

    int *stream = malloc(N * sizeof(int));
    for (int t = 0; t < N; t++) {
        double r = rand01();
        // binary search: first index whose cdf >= r  (the sampled rank)
        int lo = 0, hi = K - 1, rank = K - 1;
        while (lo <= hi) {
            int mid = (lo + hi) / 2;
            if (cdf[mid] >= r) { rank = mid; hi = mid - 1; } else lo = mid + 1;
        }
        int offset = drift_span ? (int)((long)t * drift_span / N) : 0;
        stream[t] = (rank + offset) % K;
    }
    free(cdf);
    return stream;
}

// ============================================================
//  Drivers: run the identical stream through each policy
// ============================================================
double run_lru(int *stream, int N, int buckets, int capacity) {
    lru_cache *c = lru_create(buckets, capacity);
    int hits = 0; char kb[32];
    for (int i = 0; i < N; i++) {
        snprintf(kb, sizeof(kb), "%d", stream[i]);
        if (lru_get(c, kb)) hits++;
        else lru_set(c, kb, "v");
    }
    lru_free(c);
    return 100.0 * hits / N;
}

double run_lfu(int *stream, int N, int buckets, int capacity) {
    lfu_cache *c = lfu_create(buckets, capacity);
    int hits = 0; char kb[32];
    for (int i = 0; i < N; i++) {
        snprintf(kb, sizeof(kb), "%d", stream[i]);
        if (lfu_get(c, kb)) hits++;
        else lfu_set(c, kb, "v");
    }
    lfu_free(c);
    return 100.0 * hits / N;
}

void run_experiment(const char *name, int N, int K, double s, int drift, int cap) {
    int buckets = 1024;
    int *stream = gen_stream(N, K, s, drift);
    double lru = run_lru(stream, N, buckets, cap);
    double lfu = run_lfu(stream, N, buckets, cap);
    free(stream);

    printf("%-28s  LRU %5.1f%%   LFU %5.1f%%   ->  %s by %.1f pts\n",
           name, lru, lfu,
           lfu > lru ? "LFU wins" : (lru > lfu ? "LRU wins" : "tie"),
           fabs(lfu - lru));
}

int main(void) {
    srand(42);   // reproducible

    int N = 200000;   // requests
    int K = 1000;     // distinct keys
    int cap = 100;    // cache holds 10% of the keyspace

    printf("Requests=%d  Keys=%d  Capacity=%d (%.0f%% of keyspace)\n\n",
           N, K, cap, 100.0 * cap / K);

    printf("Workload                      Hit rates                    Winner\n");
    printf("--------------------------------------------------------------------------\n");
    run_experiment("static skew  (Zipf s=1.2)",  N, K, 1.2, 0,   cap);
    run_experiment("stronger skew (Zipf s=1.5)",  N, K, 1.5, 0,   cap);
    run_experiment("drifting popularity",         N, K, 1.2, 900, cap);
    printf("\n");
    return 0;
}