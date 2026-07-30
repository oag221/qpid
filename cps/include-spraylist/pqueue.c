#include <optional>
#include <utility>

#include "pqueue.h"

int Spraylist::_old_MarsagliaXOR(int seed) {
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
int Spraylist::_MarsagliaXOR(int seed) {
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

std::optional<std::pair<slkey_t, val_t>> Spraylist::lotan_shavit_delete_min_key() {
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
    
    std::pair<slkey_t, val_t> ret = std::make_pair(first->key, first->val);
    mark_node_ptrs(first);

    // unsigned int *seed = &d->seed2;
    // *seed = _MarsagliaXOR(*seed);
    // if (*seed % (d->nb_threads) == 0) {
    //   fraser_search(set, first->val, NULL, NULL);    
    // }

    // if (!first->next[0]->deleted)
      fraser_search(set, first->key, NULL, NULL);    

    return ret;
  }

std::optional<std::pair<slkey_t, val_t>> Spraylist::spray_delete_min_key() {
    unsigned int *seed = &seed_;

  #ifndef DISTRIBUTION_EXPERIMENT 
    *seed = _MarsagliaXOR(*seed);
    if (THREADS == 1 || *seed % THREADS/*/floor_log_2(THREADS)*/ == 0) { // THREADS == 1 is equivalent to Lotan-Shavit delete_min
      //d->nb_clean++;
      return lotan_shavit_delete_min_key();
    }
  #endif

    sl_node_t *cur;
    int result;
    int scanlen;
    int height = SCANHEIGHT(THREADS); 
    int scanmax = SCANMAX(THREADS); ;
    int scan_inc = SCANINC;

    cur = set->head;

    int i = height;
    int dummy = 0;
    while(1) {
      *seed = _MarsagliaXOR(*seed);
      scanlen = *seed % (scanmax+1); 

      while (dummy < THREADS*floor_log_2(THREADS)/2 && scanlen > 0) {
        dummy += (1 << i);
        scanlen--;
      }


      while (scanlen > 0 && cur->next[i]) { // Step right //here: cur->next[0], or cur->next[i]??
        cur = (sl_node_t*)unset_mark((uintptr_t)cur->next[i]);
        if (!cur->deleted) scanlen--;
      } 

      // TODO: This is probably a reasonable condition to become a 'cleaner' since the list is so small
      //       We can also just ignore it since it shouldn't happen in benchmarks?
      if (!cur->next[0]) {
        //*key = -1;
        return {}; //got to end of list
      }

      scanmax += scan_inc;

      if (i == 0) break;
      if (i <= SCANSKIP) { i = 0; continue; } // need to guarantee bottom level gets scanned
      i -= SCANSKIP;
    }

    if (cur == set->head) // still in dummy range
      return {}; // TODO: clean instead? something else?

    while (cur->deleted && cur->next[0]) {
      cur = (sl_node_t*)unset_mark((uintptr_t)cur->next[0]); // Find first non-deleted node
    } 

    if (!cur->next[0]) return {};

//   #ifdef DISTRIBUTION_EXPERIMENT 
//     *val = (cur->val);
//     *key = (cur->key);
//     return 1;
//   #endif

    result = 1;
    if (cur->deleted == 0 )
    {
      result = ATOMIC_FETCH_AND_INC_FULL(&cur->deleted);
    }
    if (result != 0) {
      //d->nb_collisions++;  
      return {}; // TODO: Retry and eventually 'clean'
    }

    auto ret = std::make_pair(cur->key, cur->val);
    // *val = (cur->val);
    // *key = (cur->key);
    mark_node_ptrs(cur);

    // if (((*seed) & 0x10)) return 1;  

    // TODO: batch deletes (this method is somewhat inefficient)
    fraser_search(set, cur->val, NULL, NULL);

    return ret; 
  }
