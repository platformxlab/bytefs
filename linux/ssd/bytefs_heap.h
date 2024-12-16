#ifndef __QUEUE_SORT_H__
#define __QUEUE_SORT_H__

#include "ftl.h"
#include "bytefs_utils.h"

struct ssd;

struct bytefs_heap {
    int count;
    int capacity;
    int64_t *key;
    void **storage;
};

void heap_create(struct bytefs_heap *heap, int capacity);
void heap_clear(struct bytefs_heap *heap);
int heap_is_empty(struct bytefs_heap *heap);
int heap_is_full(struct bytefs_heap *heap, int reserved);
void heap_insert(struct bytefs_heap *heap, int64_t key, void *item);
void heapify_bottom_top(struct bytefs_heap *heap);
void heapify_top_bottom(struct bytefs_heap *heap);
void *heap_get_min(struct bytefs_heap *heap);
void *heap_get_min_key(struct bytefs_heap *heap, int64_t *key_ret);
int heap_pop_min(struct bytefs_heap *heap);

#endif 
