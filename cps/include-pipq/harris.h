/*
 * File:
 *   harris.h
 * Author(s):
 *   Vincent Gramoli <vincent.gramoli@epfl.ch>
 * Description:
 *   Lock-free linkedlist implementation of Harris' algorithm
 *   "A Pragmatic Implementation of Non-Blocking Linked Lists" 
 *   T. Harris, p. 300-314, DISC 2001.
 *
 * Copyright (c) 2009-2010.
 *
 * harris.h is part of Synchrobench
 * 
 * Synchrobench is free software: you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation, version 2
 * of the License.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */
 

#include <assert.h>
#include <getopt.h>
#include <limits.h>
#include <pthread.h>
#include <signal.h>
#include <stdlib.h>
#include <stdio.h>
#include <sys/time.h>
#include <time.h>
#include <stdint.h>
#include <inttypes.h>
#include <atomic>

#define MEMORY_BARRIER asm volatile ("" : : : "memory")

#ifdef RBP
#include "../../rbp/message.h"
typedef Message* val__t;
#else
typedef intptr_t val__t;
#endif
typedef intptr_t k_t;

#define KEY_MIN INT_MIN
#define KEY_MAX INT_MAX
#define EMPTY -1

#define ALIGN_SIZE 64

// tests if logical deletion bit is set
#define get_logdel_ref(_p) ((void *)(((uintptr_t)(_p)) | 1))
#define get_notlogdel_ref(_p) ((void *)(((uintptr_t)(_p)) & ~1))
#define is_logdel_ref(_p) (((uintptr_t)(_p)) & 1)

// tests if moving bit is set
#define get_moving_ref(_p) ((void *)(((uintptr_t)(_p)) | 2))
#define get_notmoving_ref(_p) ((void *)(((uintptr_t)(_p)) & ~2))
#define is_moving_ref(_p) (((uintptr_t)(_p)) & 2)

// tests if marked as logically deleted OR moving
#define get_marked_reference(_p) ((void *)(((uintptr_t)(_p)) | 3))
#define get_unmarked_reference(_p) ((void *)(((uintptr_t)(_p)) & ~3))
#define is_marked_reference(_p) (((uintptr_t)(_p)) & 3)

typedef struct alignas(8) node_ {
    k_t key;
	val__t val;
    int idx;
    int zone;
	struct node_ *next;
} node__t;

struct __attribute__((__packed__)) LeaderLargest {
    node__t* largest_ptr;
    char padding[(ALIGN_SIZE - (sizeof(node__t*)))];
};

static_assert(alignof(node_) >= 4 and alignof(node_) % 4 == 0);

typedef struct intset {
	node__t *head;
    node__t *tail;
    int max_offset;
    node__t *last_log_del;
} intset_t;

node__t *new_node(k_t key, val__t val, int idx, int zone, node__t *next);
intset_t *set_new(int offset=24);
void set_destroy(intset_t *set);
long set_size(intset_t *set);
long long set_keysum(intset_t *set);
void print_set(intset_t *set);
int set_validate(intset_t *set, int largest_per_idx[96]);

/* ################################################################### *
 * ADAPTED HARRIS' LINKED LIST
 * ################################################################### */

// search methods
node__t *harris_search(intset_t *set, k_t key, node__t **left_node);
node__t *harris_search_idx(intset_t *set, int idx, int zone, node__t **left_node);
void harris_search_physdel(intset_t *set, node__t* search_node);

// interface methods
int harris_insert(intset_t *set, LeaderLargest* last_ptr, int idx, int zone, k_t key, val__t val);
val__t harris_delete_idx(intset_t *set, int idx, int zone, k_t* key);
val__t linden_delete_min(intset_t *set, k_t* del_key, int* del_idx, int* del_zone);

// methods that track a pointer to the last log deleted node - slower with one leader, faster with 4 (both cases due to numa locality + cache misses)
node__t *opt_harris_search(intset_t *set, k_t key, node__t **left_node);
node__t *opt_harris_search_idx(intset_t *set, int idx, int zone, node__t **left_node);
val__t opt_linden_delete_min(intset_t *set, k_t* del_key, int* del_idx, int* del_zone);

// combines inserting and move into one traversal
val__t harris_insert_and_move(intset_t *set, LeaderLargest* last_ptr, int idx, int zone, k_t* key_rem, k_t key_ins, val__t val);
node__t *harris_search_ins_move(intset_t *set, int idx, int zone, node__t* start_node, node__t **left_node, LeaderLargest* last_ptr);
void reset_head_ptr(intset_t *set);