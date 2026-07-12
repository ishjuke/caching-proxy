#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct entry {
    char *key;
    char *value;
    struct entry *next;   // for collision chaining
} entry;

typedef struct {
    entry **buckets;      // array of entry pointers
    int size;
} hashtable;

// djb2: start at 5381, hash = hash * 33 + c for each char, then mod by size
unsigned long hash(const char *key, int size) {
    unsigned long h = 5381;
    int c;
    while ((c = *key++)) {
        h = h * 33 + c;
    }
    return h % size;
}

// allocate a table with `size` empty buckets
hashtable *ht_create(int size) {
    hashtable *ht = malloc(sizeof(hashtable));
    if (!ht) return NULL;

    ht->size = size;
    ht->buckets = calloc(size, sizeof(entry *));  // calloc zeroes them to NULL
    if (!ht->buckets) {
        free(ht);
        return NULL;
    }
    return ht;
}

// insert or update. strdup copies the strings so the caller's memory stays independent.
void ht_set(hashtable *ht, const char *key, const char *value) {
    unsigned long index = hash(key, ht->size);

    // if the key already exists in this bucket, update its value
    entry *e = ht->buckets[index];
    while (e != NULL) {
        if (strcmp(e->key, key) == 0) {
            free(e->value);
            e->value = strdup(value);
            return;
        }
        e = e->next;
    }

    // otherwise create a new node and push it to the front of the chain
    entry *new_entry = malloc(sizeof(entry));
    new_entry->key = strdup(key);
    new_entry->value = strdup(value);
    new_entry->next = ht->buckets[index];
    ht->buckets[index] = new_entry;
}

// return the value for a key, or NULL if it's not there
char *ht_get(hashtable *ht, const char *key) {
    unsigned long index = hash(key, ht->size);

    entry *e = ht->buckets[index];
    while (e != NULL) {
        if (strcmp(e->key, key) == 0) {
            return e->value;
        }
        e = e->next;
    }
    return NULL;
}

// walk every bucket and every chain, freeing all the strings and nodes
void ht_free(hashtable *ht) {
    for (int i = 0; i < ht->size; i++) {
        entry *e = ht->buckets[i];
        while (e != NULL) {
            entry *next = e->next;   // save next before freeing e
            free(e->key);
            free(e->value);
            free(e);
            e = next;
        }
    }
    free(ht->buckets);
    free(ht);
}

int main(void) {
    hashtable *ht = ht_create(16);

    ht_set(ht, "name", "Ada");
    ht_set(ht, "lang", "C");
    ht_set(ht, "name", "Grace");   // overwrites the earlier value

    printf("name -> %s\n", ht_get(ht, "name"));   // Grace
    printf("lang -> %s\n", ht_get(ht, "lang"));   // C

    char *missing = ht_get(ht, "missing");
    printf("missing -> %s\n", missing ? missing : "(not found)");

    ht_free(ht);
    return 0;
}
