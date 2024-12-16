// #include <stdio.h>
// #include <stdlib.h>

#include "ftl.h"
#include "ftl_mapping.h"
#include "backend.h"
#include "bytefs_heap.h"


void heap_create(struct bytefs_heap *heap, int capacity) {
  heap->capacity = capacity;
  heap->key = kzalloc(capacity * sizeof(int64_t), GFP_KERNEL);
  heap->storage = kzalloc(capacity * sizeof(void *), GFP_KERNEL);
  bytefs_expect(heap->key);
  bytefs_expect(heap->storage);
  heap_clear(heap);
}

void heap_clear(struct bytefs_heap *heap) {
  bytefs_assert(heap->capacity >= 0);
  heap->count = 0;
}

int heap_is_empty(struct bytefs_heap *heap) {
  return heap->count == 0;
}

int heap_is_full(struct bytefs_heap *heap, int reserved) {
  return heap->capacity - reserved <= heap->count;
}

void heap_insert(struct bytefs_heap *heap, int64_t key, void *item) {
  bytefs_assert_msg(heap->count <= heap->capacity, 
      "Heap overflow Capacity: %d/%d", heap->count, heap->capacity);
  heap->key[heap->count] = key;
  heap->storage[heap->count] = item;
  heapify_bottom_top(heap);
  heap->count++;
}

void heapify_bottom_top(struct bytefs_heap *heap) {
  void *temp_item;
  int64_t temp_key;

  int index = heap->count;
  int pnode_idx = (index - 1) / 2;
  while (heap->key[pnode_idx] > heap->key[index]) {
    // swap items in the storage
    temp_item = heap->storage[pnode_idx];
    heap->storage[pnode_idx] = heap->storage[index];
    heap->storage[index] = temp_item;
    // swap weight
    temp_key = heap->key[pnode_idx];
    heap->key[pnode_idx] = heap->key[index];
    heap->key[index] = temp_key;
    // continue to pnode
    index = pnode_idx;
    pnode_idx = (index - 1) / 2;
  }
}

void heapify_top_bottom(struct bytefs_heap *heap) {
  int min_idx;
  void *temp_item;
  int64_t temp_key;

  int pnode_idx = 0;
  int left_idx = pnode_idx * 2 + 1;
  int right_idx = pnode_idx * 2 + 2;

  while (1) {
    if (left_idx >= heap->count || left_idx < 0) left_idx = -1;
    if (right_idx >= heap->count || right_idx < 0) right_idx = -1;

    if (left_idx != -1 && heap->key[left_idx] < heap->key[pnode_idx]) {
      min_idx = left_idx;
    } else {
      min_idx = pnode_idx;
    } 
    if (right_idx != -1 && heap->key[right_idx] < heap->key[min_idx]) {
      min_idx = right_idx;
    }
    if (min_idx != pnode_idx) {
      // swap items in the storage
      temp_item = heap->storage[pnode_idx];
      heap->storage[pnode_idx] = heap->storage[min_idx];
      heap->storage[min_idx] = temp_item;
      // swap weight
      temp_key = heap->key[pnode_idx];
      heap->key[pnode_idx] = heap->key[min_idx];
      heap->key[min_idx] = temp_key;
      // continue to pnode
      pnode_idx = min_idx;
      left_idx = pnode_idx * 2 + 1;
      right_idx = pnode_idx * 2 + 2;
    } else {
      break;
    }
  }
}

void *heap_get_min(struct bytefs_heap *heap) {
  return heap->count == 0 ? NULL : heap->storage[0];
}

void *heap_get_min_key(struct bytefs_heap *heap, int64_t *key_ret) {
  if (heap->count == 0)
    return NULL;
  *key_ret = heap->key[0];
  return heap->storage[0];
}

int heap_pop_min(struct bytefs_heap *heap) {
  if (heap->count == 0) 
    return -1;
  heap->key[0] = heap->key[heap->count - 1];
  heap->storage[0] = heap->storage[heap->count - 1];
  heap->count--;
  heapify_top_bottom(heap);
  return 0;
}
