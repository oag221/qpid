#include <optional>
#include <utility>

#include "../common/intset.h"

#define LOG3(n) floor_log_2(n)*floor_log_2(n)*floor_log_2(n)
#define LOG2(n) floor_log_2(n)*floor_log_2(n)
#define LOGLOG(n) floor_log_2(floor_log_2(n))

// SCANHEIGHT is what height to start spray at; must be >= 0
//#define SCANHEIGHT floor_log_2(n)+1
#define SCANHEIGHT floor_log_2(n)+1
// SCANMAX is scanlength at the top level; must be > 0
#define SCANMAX floor_log_2(n)+1
// SCANINC is the amount to increase scan length at each step; can be any integer
#define SCANINC 0
//SCANSKIP is # of levels to go down at each step; must be > 0
#define SCANSKIP 1


static int _old_MarsagliaXOR(int seed) {
  const int a =      16807;
  const int m = 2147483647;
  const int q =     127773;  /* m div a */
  const int r =       2836;  /* m mod a */
  int hi   = seed / q;
  int lo   = seed % q;
  int test = a * lo - r * hi;
  if (test > 0)
    seed = test;
  else
    seed = test + m;
  
  return seed;
}
static int _MarsagliaXOR(int seed) {
//   const int a =      16807;
  const int a =      123456789;
  const int m =     2147483647;
  const int q =      521288629;  /* m div a */
  const int r =       88675123;  /* m mod a */
  int hi   = seed / q;
  int lo   = seed % q;
  int test = a * lo - r * hi;
  if (test > 0)
    seed = test;
  else
    seed = test + m;

  return seed;
}

std::optional<std::pair<slkey_t,val_t>> lotan_shavit_delete_min(sl_intset_t *set) {
  return lotan_shavit_delete_min_key(set);
}

std::optional<std::pair<slkey_t,val_t>> lotan_shavit_delete_min_key(sl_intset_t *set, int order) {
  sl_node_t *first;

  first = set->head;

  while(1) {
    do {
      first = (sl_node_t*)unset_mark((uintptr_t)first->next[0]);
    } while(first->next[0] && first->deleted);
   if (!(first->next[0] && ATOMIC_FETCH_AND_INC_FULL(&first->deleted) != 0)) {
     break;
   }
  }

  if (first->next[0] == NULL) {
    return {};
  }

  mark_node_ptrs(first);

  // unsigned int *seed = &d->seed2;
  // *seed = _MarsagliaXOR(*seed);
  // if (*seed % (d->nb_threads) == 0) {
  //   fraser_search(set, first->val, NULL, NULL);    
  // }

  // if (!first->next[0]->deleted)
  fraser_search(set, first->key, NULL, NULL, order);    

  return std::make_pair(first->key, first->val); 
}

std::optional<std::pair<slkey_t,val_t>> spray_delete_min(sl_intset_t *set, thread_data_t *d, int order) {
  return spray_delete_min_key(set, d, order);
}

std::optional<std::pair<slkey_t,val_t>> spray_delete_min_key(sl_intset_t *set, thread_data_t *d, int order) {
  unsigned int n = d->nb_threads;
  unsigned int *seed = &d->seed2;

#ifndef DISTRIBUTION_EXPERIMENT 
  *seed = _MarsagliaXOR(*seed);
  if (n == 1 || *seed % n/*/floor_log_2(n)*/ == 0) { 
    d->nb_clean++;
    return lotan_shavit_delete_min_key(set, order); 
  }
#endif

  sl_node_t *cur;
  int scanlen;
  int height = SCANHEIGHT; 
  int scanmax = SCANMAX;
  int scan_inc = SCANINC;

  cur = set->head;

  int i = height;
  int dummy = 0;
  while(1) {
    *seed = _MarsagliaXOR(*seed);
    scanlen = *seed % (scanmax+1); 

    while (dummy < n*floor_log_2(n)/2 && scanlen > 0) {
      dummy += (1 << i);
      scanlen--;
    }

    while (scanlen > 0 && cur->next[i]) { 
      cur = (sl_node_t*)unset_mark((uintptr_t)cur->next[i]);
      if (!cur->deleted) scanlen--;
    } 

    if (!cur->next[0]) {
      // FIX 1a: Do not return empty. Fall back to the head-based cleaner 
      // to check if the queue is truly empty or if we just overshot.
      return lotan_shavit_delete_min_key(set, order); 
    }

    scanmax += scan_inc;

    if (i == 0) break;
    if (i <= SCANSKIP) { i = 0; continue; } 
    i -= SCANSKIP;
  }

  if (cur == set->head) 
    // FIX 1b: Fall back to cleaner instead of returning a false empty
    return lotan_shavit_delete_min_key(set, order); 

  // Infinite retry loop to handle heavy CAS contention on the bottom level
  while (true) {
    while (cur->deleted && cur->next[0]) {
      cur = (sl_node_t*)unset_mark((uintptr_t)cur->next[0]); 
    } 

    if (!cur->next[0]) {
      // FIX 1c: Hit the end of the list while walking; let the cleaner verify emptiness
      return lotan_shavit_delete_min_key(set, order); 
    }

#ifdef DISTRIBUTION_EXPERIMENT 
    *val = (cur->val);
    *key = (cur->key);
    return 1;
#endif

    // Attempt logical deletion
    if (ATOMIC_FETCH_AND_INC_FULL(&cur->deleted) == 0) {
      break; // Success! We safely claimed this node.
    }
    
    // Lost the CAS race to a concurrent thread. 
    // Loop repeats, and the 'while(cur->deleted)' block above will automatically step us right.
  }

  // Logically mark forward pointers to prepare for physical unlinking
  mark_node_ptrs(cur);

  // FIX 2: Force fraser_search to scan PAST all identical duplicate keys.
  // This guarantees our marked node is actually encountered and unlinked,
  // preventing memory leaks and severe linear scan degradation.
  slkey_t cleanup_key = order ? (cur->key - 1) : (cur->key + 1);
  fraser_search(set, cleanup_key, NULL, NULL, order);

  return std::make_pair(cur->key, cur->val); 
}


// std::optional<std::pair<slkey_t,val_t>> spray_delete_min_key(sl_intset_t *set, thread_data_t *d, int order) {
//   unsigned int n = d->nb_threads;
//   unsigned int *seed = &d->seed2;

// #ifndef DISTRIBUTION_EXPERIMENT 
//   *seed = _MarsagliaXOR(*seed);
//   if (n == 1 || *seed % n/*/floor_log_2(n)*/ == 0) { // n == 1 is equivalent to Lotan-Shavit delete_min
//     d->nb_clean++;
//     return lotan_shavit_delete_min_key(set, order); // todo: add "order" here
//   }
// #endif

//   sl_node_t *cur;
//   int scanlen;
//   int height = SCANHEIGHT; 
//   int scanmax = SCANMAX;
//   int scan_inc = SCANINC;

//   cur = set->head;

//   int i = height;
//   int dummy = 0;
//   while(1) {
//     *seed = _MarsagliaXOR(*seed);
//     scanlen = *seed % (scanmax+1); 

//     while (dummy < n*floor_log_2(n)/2 && scanlen > 0) {
//       dummy += (1 << i);
//       scanlen--;
//     }


//     while (scanlen > 0 && cur->next[i]) { // Step right //here: cur->next[0], or cur->next[i]??
//       cur = (sl_node_t*)unset_mark((uintptr_t)cur->next[i]);
//       if (!cur->deleted) scanlen--;
//     } 

//     // TODO: This is probably a reasonable condition to become a 'cleaner' since the list is so small
//     //       We can also just ignore it since it shouldn't happen in benchmarks?
//     if (!cur->next[0]) {
//       return {}; //got to end of list
//     }

//     scanmax += scan_inc;

//     if (i == 0) break;
//     if (i <= SCANSKIP) { i = 0; continue; } // need to guarantee bottom level gets scanned
//     i -= SCANSKIP;
//   }

//   if (cur == set->head) // still in dummy range
//     return {}; // TODO: clean instead? something else?

//   while (true) {
//     while (cur->deleted && cur->next[0]) {
//       cur = (sl_node_t*)unset_mark((uintptr_t)cur->next[0]); // Find first non-deleted node
//     } 

//     if (!cur->next[0]) return {};

//   #ifdef DISTRIBUTION_EXPERIMENT 
//     *val = (cur->val);
//     *key = (cur->key);
//     return 1;
//   #endif

//     //int result = 1;
//     if (cur->deleted == 0 && ATOMIC_FETCH_AND_INC_FULL(&cur->deleted) == 0) {
//       //result = ATOMIC_FETCH_AND_INC_FULL(&cur->deleted);
//       break;
//     }
//     // if (result != 0) {
//     //   //d->nb_collisions++;  
//     //   return {}; // TODO: Retry and eventually 'clean'
//     // }
//   }*/

//   mark_node_ptrs(cur);

//   // if (((*seed) & 0x10)) return 1;  

//   // TODO: batch deletes (this method is somewhat inefficient)
//   //! (set, cur->val, NULL, NULL, order); // todo OG[2-10-26]: why passing cur->val here..?
//   fraser_search(set, cur->key, NULL, NULL, order);

//   return std::make_pair(cur->key, cur->val); 
// }
