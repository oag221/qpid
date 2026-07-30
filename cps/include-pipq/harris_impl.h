/*
 * File: harris.cc
 * Original File: harris.c
 * Author(s):
 *   Vincent Gramoli <vincent.gramoli@epfl.ch> - Original (harris.c)
 * 		Description:
 *   	Lock-free linkedlist implementation of Harris' algorithm
 *   	"A Pragmatic Implementation of Non-Blocking Linked Lists" 
 * 		T. Harris, p. 300-314, DISC 2001.
 *
 * Copyright (c) 2009-2010.
 *
 * harris.c is part of Synchrobench
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

#include <iostream>
#include "harris.h"

int cur_offset = 0;
node__t *last_deleted = nullptr;

node__t *new_node(k_t key, val__t val, int idx, int zone, node__t *next) {
	node__t *node = (node__t *)malloc(sizeof(node__t));
	if (node == NULL) {
		perror("malloc");
		exit(1);
	}
	node->key = key;
	node->val = val;
	node->idx = idx;
	node->zone = zone;
	node->next = next;

	return node;
}

intset_t *set_new(int offset) {
  intset_t *set;
  node__t *min, *max;
	
  if ((set = (intset_t *)malloc(sizeof(intset_t))) == NULL) {
    perror("malloc");
    exit(1);
  }
  #ifdef RBP
  max = new_node(KEY_MAX, nullptr, EMPTY, EMPTY, nullptr);
  min = new_node(KEY_MIN, nullptr, EMPTY, EMPTY, max);
  #else
  max = new_node(KEY_MAX, EMPTY, EMPTY, EMPTY, nullptr);
  min = new_node(KEY_MIN, EMPTY, EMPTY, EMPTY, max);
  #endif
  set->head = min;
  set->tail = max;
  set->max_offset = offset;
  set->last_log_del = nullptr;

  return set;
}

void set_destroy(intset_t *set) {
  node__t *node, *next;

  node = set->head;
  while (node != NULL) {
	if (is_marked_reference(node)) {
		node__t* tmp = (node__t*)get_unmarked_reference(node);
		next = tmp->next;
		free(tmp);
		node = next;
	} else {
		next = node->next;
    	free(node);
    	node = next;
	}
  }
  free(set);
}

long set_size(intset_t *set) {
  long size = 0;
  node__t *node;

  int num_logdel = 0;
  int num_moved = 0;

  /* We have at least 2 elements */
  node = set->head->next;
  while ((node__t*)get_unmarked_reference(node) != set->tail) {
	if (is_marked_reference(node)) {
		
		node__t* tmp = (node__t*)get_unmarked_reference(node);
		if (is_moving_ref(node)) {
			num_moved++;
			std::cout << "\t(" << tmp->idx << ") " << tmp->key << " (moving)\n";
		} else {
			num_logdel++;
			std::cout << "\t(" << tmp->idx << ") " << tmp->key << " (log_del)\n";
		}
		node = tmp->next;
	} else {
		size++;
    	node = node->next;
	}
  }
  std::cout << "\nnum logically deleted nodes: " << num_logdel << "\n";
  std::cout << "num moving nodes: " << num_moved << "\n\n";
  return size;
}

long long set_keysum(intset_t *set) {
  long long sum = 0;
  node__t *node;

  int cnt = 0;

  /* We have at least 2 elements */
  node = set->head->next;
  while ((node__t*)get_unmarked_reference(node) != set->tail) {
	if (is_marked_reference(node)) {
		node__t* tmp = (node__t*)get_unmarked_reference(node);
		node = tmp->next;
	} else {
		cnt++;
		sum += node->key;
    	node = node->next;
	}
  }
  return sum;
}

int set_validate(intset_t *set, int largest_per_idx[96]) {
  node__t *node;
  long long cur;
  int prev_key = -2;
  int num_incorrect = 0;

  int cnt = 0;

  int myArray[96] = { 0 };

  /* We have at least 2 elements */
  node = set->head->next;
  while ((node__t*)get_unmarked_reference(node) != set->tail) {
	if (is_marked_reference(node)) {
		node__t* tmp = (node__t*)get_unmarked_reference(node);
		node = tmp->next;
	} else {
		cnt++;
		cur = node->key;

		myArray[node->zone * 24 + node->idx]++;
		//std::cout << "[tid=" << node->zone * 24 + node->idx << "] " << cur << " {" << myArray[node->zone * 24 + node->idx] << "}\n";

		largest_per_idx[node->zone * 24 + node->idx] = cur;
		if (prev_key > cur) {
			std::cout << "INCORRECT ORDER! prev_key = " << prev_key << ", cur_key = " << cur << "\n";
			num_incorrect++;
		}
		prev_key = cur;
    	node = node->next;
	}
  }
  std::cout << "Size of list: " << cnt << "\n";
  return num_incorrect;
}

void print_set(intset_t *set) {
  node__t *node;

  /* We have at least 2 elements */
  node = set->head->next;
  std::cout << "\n";
  while ((node__t*)get_unmarked_reference(node) != set->tail) {
	if (is_marked_reference(node)) {
		node__t* tmp = (node__t*)get_unmarked_reference(node);
		if (is_logdel_ref(node)) {
			std::cout << "(" << tmp->idx <<  ") " << tmp->key << "[LOG-DEL]\n";
		}
		if (is_moving_ref(node)) {
			std::cout << "(" << tmp->idx <<  ") " << tmp->key << "[MOVING]\n";
		}
		node = tmp->next;
	} else {
		std::cout << "(" << node->idx <<  ") " << node->key << "\n";
		node = node->next;
	}
  }
  std::cout << "\n\n";
}


/* --------------------------------------------------- */
/* --------------------------------------------------- */
/*                      METHODS                        */
/* --------------------------------------------------- */
/* --------------------------------------------------- */


node__t *harris_search_idx(intset_t *set, int idx, int zone, node__t **left_node) {
	node__t *left_node_next, *right_node, *cur_left_node, *cur_left_node_next;
	left_node_next = set->head;
	
 search_again:
	do {
		node__t *x = set->head;
		node__t *x_next = x->next;
		
		/* 1. Find left_node and right_node */
		do {
			if (!is_moving_ref(x_next)) {
				cur_left_node = x;
				cur_left_node_next = (node__t *)get_notlogdel_ref(x_next); // in case x_next as logically deleted
			}
			x = (node__t *) get_unmarked_reference(x_next);
			if (!x->next) break;
			x_next = x->next;

			if (!is_moving_ref(x_next) && x->idx == idx && x->zone == zone) {
				(*left_node) = cur_left_node;
				left_node_next = cur_left_node_next;
				right_node = x;
			}
		} while (1);

		if (!right_node) {
			return nullptr;
		}
		
		/* 2. Check that nodes are adjacent */
		if (left_node_next == right_node) return right_node;
		
		/* 3. Remove one or more marked nodes */
		if (__sync_bool_compare_and_swap(&(*left_node)->next, left_node_next, right_node)) return right_node;
	} while (1);
}

node__t *harris_search_ins_move(intset_t *set, int idx, int zone, node__t* start_node, node__t **left_node, node__t* starting_last_ptr, LeaderLargest* last_ptr) {
	node__t *left_node_next, *right_node, *cur_left_node, *cur_left_node_next;
	left_node_next = set->head;

	node__t *x = start_node;
	node__t *x_next = x->next;
	right_node = start_node;

	node__t* new_last_ptr;
	
 search_again:
	do {
		/* 1. Find left_node and right_node */
		do {
			if (!is_moving_ref(x_next)) {
				cur_left_node = x;
				cur_left_node_next = (node__t *)get_notlogdel_ref(x_next); // in case x_next as logically deleted
			}
			x = (node__t *) get_unmarked_reference(x_next);
			if (!x->next) break;
			x_next = x->next;

			if (!is_moving_ref(x_next) && x->idx == idx && x->zone == zone) {
				new_last_ptr = right_node;
				(*left_node) = cur_left_node;
				left_node_next = cur_left_node_next;
				right_node = x;
				if (x == starting_last_ptr) {
					break;
				}
			}
		} while (1);

		last_ptr->largest_ptr = new_last_ptr;

		if (!right_node) {
			return nullptr;
		}
		
		/* 2. Check that nodes are adjacent */
		if (left_node_next == right_node) return right_node;
		
		/* 3. Remove one or more marked nodes */
		if (__sync_bool_compare_and_swap(&(*left_node)->next, left_node_next, right_node)) return right_node;

		x = set->head;
		x_next = x->next;
		right_node = new_last_ptr;
	} while (1);
}

node__t *harris_search(intset_t *set, k_t key, node__t **left_node) { // left_node init to point to head
	node__t *left_node_next, *right_node;
	
 search_again:
	do {
		bool prev_logdel; // to traverse past deleted nodes

		node__t *x = set->head;
		node__t *x_next = x->next;

		do {
			if (!is_moving_ref(x_next)) {
				*left_node = x;
				left_node_next = (node__t *)get_notlogdel_ref(x_next); // might be marked as logically deleted
			}
			x = (node__t *)get_unmarked_reference(x_next);
			if (x == set->tail) break;
			prev_logdel = is_logdel_ref(x_next);
			x_next = x->next;
		}
		#ifdef ORDER_DECREASING
		while (x->key > key || is_moving_ref(x_next) || prev_logdel);
		#else
		while (x->key < key || is_moving_ref(x_next) || prev_logdel);
		#endif

		right_node = x;
		
		/* 2. Check that nodes are adjacent */
		if (left_node_next == right_node) {
			if ((right_node->next && is_moving_ref(right_node->next)) || is_logdel_ref((*left_node)->next)) // latter condition makes sure right node has not since been deleted by del-min op
				goto search_again;
			else return right_node;
		}
		
		/* 3. Remove one or more marked nodes */
		if (__sync_bool_compare_and_swap(&(*left_node)->next, 
						  left_node_next, 
						  right_node)) {
			if ((right_node->next && is_moving_ref(right_node->next)) || is_logdel_ref((*left_node)->next))
				goto search_again;
			else return right_node;
		}
	} while (1);
}

void harris_search_physdel(intset_t *set, node__t* search_node) { // left_node init to point to head
	node__t *left_node_next, *left_node, *right_node;
	
 search_again:
	do {
		bool prev_logdel; // to traverse past deleted nodes
		bool found = false;

		node__t *x = set->head;
		node__t *x_next = x->next;

		do {
			if (!is_moving_ref(x_next)) {
				left_node = x;
				left_node_next = (node__t *)get_notlogdel_ref(x_next); // might be marked as logically deleted
			}
			x = (node__t *)get_unmarked_reference(x_next);

			if (x == set->tail) break;
			if (x == search_node) found = true;
			
			prev_logdel = is_logdel_ref(x_next);
			x_next = x->next;
		} while (!found || is_moving_ref(x_next) || prev_logdel);

		if (!found) return; // has been deleted by another thread

		right_node = x;
		
		/* 2. Remove one or more marked nodes */
		if (__sync_bool_compare_and_swap(&left_node->next, left_node_next, right_node)) 
			return;
	} while (1);
}

/*3
 * harris_find inserts a new node with the given priority key and value val in the list
 * (if absent) or does nothing (if the key-value pair is already present).
 */
int harris_insert(intset_t *set, LeaderLargest* last_ptr, int idx, int zone, k_t key, val__t val) {
	node__t *newnode, *right_node, *left_node;
	left_node = set->head;
	
	do {
		right_node = harris_search(set, key, &left_node);
		newnode = new_node(key, val, idx, zone, right_node);
		/* mem-bar between node creation and insertion */
		MEMORY_BARRIER;
		if (__sync_bool_compare_and_swap(&left_node->next, right_node, newnode)) {
			#ifdef ORDER_DECREASING
			if (!(last_ptr->largest_ptr) || key < (last_ptr->largest_ptr)->key) last_ptr->largest_ptr = newnode;
			#else
			if (!(last_ptr->largest_ptr) || key > (last_ptr->largest_ptr)->key) last_ptr->largest_ptr = newnode;
			#endif
			return 1;
		}
		free(newnode);
	} while(1);
}

val__t harris_insert_and_move(intset_t *set, LeaderLargest* last_ptr, int idx, int zone, k_t* key_rem, k_t key_ins, val__t val) {
	node__t *newnode, *right_node, *left_node, *right_node_next;
	left_node = set->head;

	node__t* starting_last_ptr = last_ptr->largest_ptr;

	// if (starting_last_ptr) {
	// 	assert(key_ins < starting_last_ptr->key);
	// }
	
	// insert the new node
	do {
		right_node = harris_search(set, key_ins, &left_node);
		newnode = new_node(key_ins, val, idx, zone, right_node);
		/* mem-bar between node creation and insertion */
		MEMORY_BARRIER;
		if (__sync_bool_compare_and_swap(&left_node->next, right_node, newnode)) {
			break; // now time for move
		}
		free(newnode);
	} while(1);

	// traverse from the new node to (*last_node) and remove it, resetting *last_node along the way
	do {
		right_node = harris_search_ins_move(set, idx, zone, newnode, &left_node, starting_last_ptr, last_ptr); // todo: if we retry (below) should reset left_node and newnode ?
		if (right_node == set->tail) {
			//std::cout << "Returning empty!\n";
			*key_rem = EMPTY;
			#ifdef RBP
			return nullptr;
			#else
			return EMPTY;
			#endif
		}
		assert(right_node->idx == idx);

		right_node_next = right_node->next;

		if (!is_marked_reference(right_node_next)) {
			if (__sync_bool_compare_and_swap(&right_node->next, right_node_next, (struct node_ *)get_moving_ref(right_node_next))) {
				// indicates to concurrent insertions that we are moving this node
				break;
			}
		}
	} while(1);
	
	*key_rem = right_node->key;
	val__t ret_val = right_node->val;

	if (!__sync_bool_compare_and_swap(&left_node->next, right_node, (struct node_ *)right_node_next)) {
		harris_search_physdel(set, right_node);
	}
	return ret_val;
}

val__t harris_delete_idx(intset_t *set, int idx, int zone, k_t* key) {
	node__t *right_node, *right_node_next, *left_node;// *left_node_next, *old_right_node;
	left_node = set->head;
	
	do {
		right_node = harris_search_idx(set, idx, zone, &left_node);

		if (right_node == set->tail) {
			*key = EMPTY;
			#ifdef RBP
			return nullptr;
			#else
			return EMPTY;
			#endif
		}
		assert(right_node->idx == idx);

		right_node_next = right_node->next;

		if (!is_marked_reference(right_node_next)) {
			if (__sync_bool_compare_and_swap(&right_node->next, right_node_next, (struct node_ *)get_moving_ref(right_node_next))) {
				// indicates to concurrent insertions that we are moving this node
				break;
			}
		}
	} while(1);

	*key = right_node->key;
	val__t ret_val = right_node->val;

	if (!__sync_bool_compare_and_swap(&left_node->next, right_node, right_node_next)) {
		harris_search_physdel(set, right_node);
	}
	return ret_val;
}

val__t linden_delete_min(intset_t *set, k_t* del_key, int* del_idx, int* del_zone) {
	node__t *x, *x_next; //, *new_head; //, *obs_head;

	#ifdef RBP
	val__t ret_val = nullptr;
	#else
	val__t ret_val = EMPTY;
	#endif
	int offset = 0;

	x = set->head;
	//obs_head = set->head->next;
	
	/* Find left_node and right_node */
	do {
		offset++;
		x_next = x->next;

		if (get_notlogdel_ref(x_next) == set->tail) {
			*del_key = EMPTY;
			return ret_val; 
		}

		if (is_logdel_ref(x_next))
			continue;

		// lin pt.
        x_next = (struct node_ *)__sync_fetch_and_or((uintptr_t *)&x->next, 1);
		//x_next = (struct node_ *)__sync_fetch_and_or(&x->next, 1);
	} while ((x = (node__t *)get_notlogdel_ref(x_next)) && is_logdel_ref(x_next));
	//new_head = x;

	*del_key = x->key;
	*del_idx = x->idx;
	*del_zone = x->zone;
	ret_val = x->val;
	
	// check if we should perform physical deletion
	if (offset >= set->max_offset) {
		//set->head->next = (node__t *)get_logdel_ref(new_head);
		set->head->next = (node__t *)get_logdel_ref(x);
		/* -- uncomment if more than one thread performs del-min concurrently
		if (set->head->next == obs_head) {
			__sync_bool_compare_and_swap(&set->head->next, obs_head, get_logdel_ref(new_head))); // either we succeed, or someone else does
		}
		*/
	}
	return ret_val;
}