#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct entry {
    char *key;
    char *value;

    struct entry *hnext;   // hash bucket collision chain

    struct entry *prev;    // recency list: toward head (more recent)
    struct entry *next;    // recency list: toward tail (less recent)
} entry;

typedef struct {
    entry **buckets;       // hash table
    int size;              // number of buckets

    entry *head;           // most recently used
    entry *tail;           // least recently used

    int count;             // current number of entries
    int capacity;          // max entries before eviction
} lru_cache;

// djb2
unsigned long hash(const char *key, int size) {
    unsigned long h = 5381;
    int c;
    while ((c = *key++)) {
        h = h * 33 + c;
    }
    return h % size;
}

// create a cache with `size` buckets and a max of `capacity` entries
lru_cache *cache_create(int size, int capacity) {
    lru_cache *c = malloc(sizeof(lru_cache));
    if (!c) return NULL;

    c->buckets = calloc(size, sizeof(entry *));  // all NULL
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

// --- recency-list helpers (unchanged, they're correct) ---

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

// --- eviction: drop the least-recently-used entry (the tail) ---

void cache_evict(lru_cache *c) {
    entry *victim = c->tail;
    if (!victim) return;   // nothing to evict

    // 1. unlink from the recency (doubly-linked) list
    list_remove(c, victim);

    // 2. unlink from its hash bucket (singly-linked hnext chain).
    //    trailing-pointer pattern: walk with prev one step behind cur.
    unsigned long idx = hash(victim->key, c->size);
    entry *cur = c->buckets[idx];
    entry *prev = NULL;
    while (cur != victim) {   // victim is guaranteed to be in this bucket
        prev = cur;
        cur = cur->hnext;
    }
    if (prev == NULL) {
        c->buckets[idx] = cur->hnext;   // victim was first in the chain
    } else {
        prev->hnext = cur->hnext;       // splice past victim
    }

    // 3. free it and update the count
    free(victim->key);
    free(victim->value);
    free(victim);
    c->count--;
}

// --- get: look up a key, and bump it to most-recently-used on a hit ---

char *cache_get(lru_cache *c, const char *key) {
    unsigned long idx = hash(key, c->size);

    entry *e = c->buckets[idx];
    while (e != NULL) {
        if (strcmp(e->key, key) == 0) {
            // just accessed -> move to front of recency list
            list_remove(c, e);
            list_push_front(c, e);
            return e->value;
        }
        e = e->hnext;
    }
    return NULL;   // not found
}

// --- set: insert or update, then evict if we're over capacity ---

void cache_set(lru_cache *c, const char *key, const char *value) {
    unsigned long idx = hash(key, c->size);

    // does the key already exist? walk the hash chain
    entry *e = c->buckets[idx];
    while (e != NULL) {
        if (strcmp(e->key, key) == 0) {
            free(e->value);
            e->value = strdup(value);
            list_remove(c, e);       // update recency
            list_push_front(c, e);
            return;
        }
        e = e->hnext;
    }

    // new entry
    entry *new_entry = malloc(sizeof(entry));
    new_entry->key = strdup(key);
    new_entry->value = strdup(value);

    // front-push into the hash bucket chain
    new_entry->hnext = c->buckets[idx];
    c->buckets[idx] = new_entry;

    // front-push into the recency list (this sets prev/next for us)
    list_push_front(c, new_entry);
    c->count++;

    // insert first, THEN check — otherwise you're off by one
    if (c->count > c->capacity) {
        cache_evict(c);
    }
}

// --- free everything ---

void cache_free(lru_cache *c) {
    entry *e = c->head;
    while (e != NULL) {
        entry *next = e->next;   // save before freeing
        free(e->key);
        free(e->value);
        free(e);
        e = next;
    }
    free(c->buckets);
    free(c);
}

int main(void) {
    lru_cache *c = cache_create(16, 3);   // 16 buckets, holds 3 entries

    cache_set(c, "a", "1");
    cache_set(c, "b", "2");
    cache_set(c, "c", "3");
    // recency (MRU -> LRU): c, b, a

    printf("a -> %s\n", cache_get(c, "a"));   // hit; bumps "a" to front
    // recency now: a, c, b

    cache_set(c, "d", "4");                   // count hits 4 > 3 -> evict tail "b"
    // recency now: d, a, c

    // get once, store, then test — cache_get has a side effect (bumps recency),
    // so never call it twice to check-then-use.
    char *bval = cache_get(c, "b");
    printf("b -> %s\n", bval ? bval : "(evicted)");

    char *cval = cache_get(c, "c");
    printf("c -> %s\n", cval ? cval : "(evicted)");   // still here

    char *dval = cache_get(c, "d");
    printf("d -> %s\n", dval ? dval : "(evicted)");   // still here

    cache_free(c);
    return 0;
}