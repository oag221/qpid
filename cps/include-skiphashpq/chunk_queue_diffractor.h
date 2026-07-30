// 2024-11-15: This data structure is functional.  There are a few outstanding
// issues:
//
// -  It's not 100% clear that destructing chunk_queue is correct
// -  There are probably some optimization opportunities, especially with regard
//    to read-for-write
// -  Does the assumption about non-emptiness block a re_get optimization in
//    dequeue()?
// -  Would we be better served by having dequeue() return a pair, including
//    information about emptiness?

#pragma once

#include "random_num.h"
#include "chunk_pool.h"

#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <utility>
#include <optional>


//!
//const int NUM_QUEUES = 2; // The total number of queues

/// A FIFO queue with coalescing to avoid excess allocations.  This queue
/// supports enqueue(), dequeue(), and empty() operations.
///
/// A key invariant is that there will never be an "empty chunk" in the queue
///
/// @tparam Q_T       The type of queue stored in each array slot
/// @tparam OPTSTM  A thread descriptor type
template <typename Q_T, typename P, typename E, class OPTSTM> class chunk_queue_diffractor {
  using ownable_t = typename OPTSTM::ownable_t;
  template <typename T> using FIELD = typename OPTSTM::template xField<T>;
  using ROTX = typename OPTSTM::RO;
  using RWTX = typename OPTSTM::RW;
 #ifdef CHUNK_POOL
  using pool_t = chunk_pool<P, E, OPTSTM>;
 #endif

public:

  struct q_ptr_t : ownable_t {
    FIELD<uint8_t> num_queues; // the number of non-empty queues (0-MAX_QUEUES)
    FIELD<uint8_t> open_lanes; // number of lanes (queues) that an insert may insert to (1-MAX_QUEUES)
    FIELD<Q_T*> *q_ptr;
    
    q_ptr_t(RWTX &rw, uint32_t chunksize, int max_queues) : ownable_t(), num_queues(0), open_lanes(1) {
      q_ptr = (FIELD<Q_T*>*)malloc(max_queues * sizeof(FIELD<Q_T*>));
    }

    ~q_ptr_t() {
      free(q_ptr);
    }
  };

  const int MAX_QUEUES;
  inline static thread_local random_num rand_engine;

  Q_T *queues;
  q_ptr_t q_ptrs;

  /// Default construct a queue from within a RW transactional context
  ///
  /// @param rw         The currently active RW transaction
  /// @param chunksize  The size of each chunk in the queue
  chunk_queue_diffractor(RWTX &rw, uint32_t chunksize, uint32_t n_queues) :
        MAX_QUEUES(n_queues),
        q_ptrs(rw, chunksize, n_queues) {
    //queues = new Q_T[n_queues];
    queues = (Q_T *)malloc(n_queues * sizeof(Q_T));
    //if (n_queues != 32) std::cout << "[chunk_queue_diffractor] num_queues is " << n_queues << "\n";
    for (int i = 0; i < n_queues; i++) {
      new (&queues[i]) Q_T(rw, chunksize);
      if (!q_ptrs.q_ptr[i].set(rw, &q_ptrs, &(queues[i]))) exit(0);
    }
  }

  ~chunk_queue_diffractor() {
    free(queues);
  }

  uint64_t random_number(int max_k) {
    std::uniform_int_distribution<int> rng_(0, max_k);
    return rng_(rand_engine);
  }

  template <typename TX> std::optional<uint64_t> random_queue_idx(TX &tx) {
    // Get current number of queues
    auto cur_num_queues_o = q_ptrs.num_queues.get(tx, &q_ptrs);
    if (!cur_num_queues_o) return {}; // ABORT!
    #ifdef PROFILING
    if (!cur_num_queues_o.value()) {
      std::cout << "NUM QUEUES IS 0\n";
      return {};
    }
    #endif
    uint8_t cur_num_queues = cur_num_queues_o.value();

    // Generate a random queue to remove from
    uint64_t rand_queue_idx = 0;
    if (cur_num_queues > 1) {
      rand_queue_idx = random_number(cur_num_queues - 1);
    }
    return rand_queue_idx;
  }

  /// Insert an element into the queue
  ///
  /// Invariant: This never leaves an empty non-sentinel node in the queue
  ///
  /// @param rw       The current active transaction
  /// @param element  The element to insert
  ///
  /// @return True if the element was inserted, false on abort
 #ifdef CHUNK_POOL
  bool enqueue(RWTX &rw, pool_t& pool, bool &used_pool, P prio, E &element)
 #else
  bool enqueue(RWTX &rw, P prio, E &element)
 #endif
  {
    // Get current number of lanes
    auto cur_num_lanes_o = q_ptrs.open_lanes.get(rw, &q_ptrs);
    if (!cur_num_lanes_o) return {}; // ABORT!
    auto cur_num_lanes = cur_num_lanes_o.value();

    // Generate a random queue to insert to
    uint64_t rand_queue_idx = 0;
    if (cur_num_lanes > 1) {
      rand_queue_idx = random_number(cur_num_lanes - 1);
    }

    // Get the proper queue
    auto queue_o = q_ptrs.q_ptr[rand_queue_idx].get(rw, &q_ptrs);
    if (!queue_o) return {}; // ABORT!
    auto queue = queue_o.value();

    bool check = (cur_num_lanes < MAX_QUEUES) && (rand_queue_idx == (cur_num_lanes - 1));
    
    // Call enqueue on the queue
   #ifdef CHUNK_POOL
    auto enq_ret_o = queue->enqueue(rw, pool, used_pool, prio, element, check);
   #else
    auto enq_ret_o = queue->enqueue(rw, prio, element, check);
   #endif
    if (!enq_ret_o) return {}; // ABORT!
    auto enq_ret = enq_ret_o.value();
    
    // (enq_ret == true) indicates to increment num_queues
    if (enq_ret) {
      auto n_queues_o = q_ptrs.num_queues.get(rw, &q_ptrs);
      if (!n_queues_o) return {}; // ABORT!
      auto n_queues = n_queues_o.value();

      if (!q_ptrs.num_queues.set(rw, &q_ptrs, n_queues + 1)) return {}; // ABORT!
    }

    // Check if a new lane should be opened
    if (check) {
      if (!q_ptrs.open_lanes.set(rw, &q_ptrs, cur_num_lanes + 1)) return {}; // ABORT!
    }
    return true;
  }

  /// futureTODO: currently inserting whole batch to a single queue; consider load balancing among queues?
  ///
  /// Insert a batch of element(s) to a randomly selected, active chunk queue
  ///
  /// @param rw       The current active transaction
  /// @param batch        The vector of elements to be inserted
  /// @param batch_size   The size of the vector
  ///
  /// @return True if the element was inserted, NONE on abort
 #ifdef CHUNK_POOL
  std::optional<bool> enqueue_batch_vec(RWTX &rw, pool_t& pool, bool &used_pool, std::vector<typename Q_T::q_node_t::kv_t> &batch, int batch_size) {
 #else
  std::optional<bool> enqueue_batch_vec(RWTX &rw, std::vector<typename Q_T::q_node_t::kv_t> &batch, int batch_size) {
 #endif
    // Get current number of lanes
    auto cur_num_lanes_o = q_ptrs.open_lanes.get(rw, &q_ptrs);
    if (!cur_num_lanes_o) return {}; // ABORT!
    auto cur_num_lanes = cur_num_lanes_o.value();

    // Generate a random queue to insert to
    uint64_t rand_queue_idx = 0;
    if (cur_num_lanes > 1) {
      rand_queue_idx = random_number(cur_num_lanes - 1);
    }

    // Get the proper queue
    auto queue_o = q_ptrs.q_ptr[rand_queue_idx].get(rw, &q_ptrs);
    if (!queue_o) return {}; // ABORT!
    auto queue = queue_o.value();

    bool check = (cur_num_lanes < MAX_QUEUES) && (rand_queue_idx == (cur_num_lanes - 1));
    
    // Call enqueue on the queue
   #ifdef CHUNK_POOL
    auto enq_ret_o = queue->enqueue_batch_vec(rw, pool, used_pool, batch, batch_size, check);
   #else
    auto enq_ret_o = queue->enqueue_batch_vec(rw, batch, batch_size, check);
   #endif
    if (!enq_ret_o) return {}; // ABORT!
    auto enq_ret = enq_ret_o.value();
    
    // (enq_ret == true) indicates to increment num_queues
    if (enq_ret) {
      auto n_queues_o = q_ptrs.num_queues.get(rw, &q_ptrs);
      if (!n_queues_o) return {}; // ABORT!
      auto n_queues = n_queues_o.value();

      if (!q_ptrs.num_queues.set(rw, &q_ptrs, n_queues + 1)) return {}; // ABORT!
    }

    // Check if a new lane should be opened
    if (check) {
      if (!q_ptrs.open_lanes.set(rw, &q_ptrs, cur_num_lanes + 1)) return {}; // ABORT!
    }
    return true;
  }

  /// Remove a chunk from the queue
  ///
  /// Invariant: This never leaves an empty non-sentinel node in the queue
  ///
  /// @param rw The current active transaction
  ///
  /// @return A chunk from the queue, or NONE on abort
  std::optional<typename Q_T::q_node_t*> dequeue(RWTX &rw, bool &empty_q) {
    // Get a random queue (idx) to insert to
    auto rand_queue_idx_o = random_queue_idx(rw);
    if (!rand_queue_idx_o) return {}; // ABORT!
    auto rand_queue_idx = rand_queue_idx_o.value();

    // Read the queue
    bool empty_rand_q = false;
    auto queue_o = q_ptrs.q_ptr[rand_queue_idx].get(rw, &q_ptrs);
    if (!queue_o) return {};
    auto queue = queue_o.value();
    
    // Perform dequeue
    auto ret_o = queue->dequeue(rw, empty_rand_q);
    if (!ret_o) return {}; // ABORT!
    auto target = ret_o.value();

    // Handle the case in which the queue is now empty
    if (empty_rand_q) {
      // NOTE: next line was read in random_queue_idx() - can use get_mine()?
      auto cur_num_queues_o = q_ptrs.num_queues.get(rw, &q_ptrs);
      if (!cur_num_queues_o || !cur_num_queues_o.value()) return {}; // ABORT! // todo: can it be 0 for cur_num_queues_o.value()..?
      uint8_t cur_num_queues = cur_num_queues_o.value();
      
      // Check if we need to swap
      if (rand_queue_idx < (cur_num_queues - 1)) {
        // Swap queue at last slot to here
        if (!swap(rw, rand_queue_idx, cur_num_queues)) return {}; // ABORT!
      }

      // Read open_lanes
      auto cur_open_lanes_o = q_ptrs.open_lanes.get(rw, &q_ptrs);
      if (!cur_open_lanes_o) return {}; // ABORT!
      auto cur_open_lanes = cur_open_lanes_o.value();

      if (cur_open_lanes > 1) {
        // Decrement open_lanes
        if (!q_ptrs.open_lanes.set(rw, &q_ptrs, cur_open_lanes - 1)) return {}; // ABORT!
      }

      // Decrement num_queues
      if (!q_ptrs.num_queues.set(rw, &q_ptrs, cur_num_queues - 1)) return {}; // ABORT!

      // Check if ALL queues are empty
      empty_q = ((cur_num_queues - 1) == 0);
    }
    return target;
  }

  /// Remove an ELEMENT from the queue (enable strict behavior)
  ///
  /// Invariant: This never leaves an empty non-sentinel node in the queue
  ///
  /// @param rw The current active transaction
  ///
  /// @return A chunk from the queue, or NONE on abort
 #ifdef CHUNK_POOL
  std::optional<std::pair<P, E>> dequeue_strict(RWTX &rw, bool &empty_q, typename Q_T::q_node_t* &retired_chunk) {
 #else
  std::optional<std::pair<P, E>> dequeue_strict(RWTX &rw, bool &empty_q) {
 #endif
    // Get a random queue (idx) to insert to
    auto rand_queue_idx_o = random_queue_idx(rw);
    if (!rand_queue_idx_o) return {}; // ABORT!
    auto rand_queue_idx = rand_queue_idx_o.value();

    // Read the queue
    auto queue_o = q_ptrs.q_ptr[rand_queue_idx].get(rw, &q_ptrs);
    if (!queue_o) return {};
    auto queue = queue_o.value();
    
    // Perform dequeue
    bool empty_rand_q = false;
   #ifdef CHUNK_POOL
    auto ret_o = queue->dequeue_strict(rw, empty_rand_q, retired_chunk);
   #else
    auto ret_o = queue->dequeue_strict(rw, empty_rand_q);
   #endif
    if (!ret_o) return {}; // ABORT!
    auto p_job = ret_o.value();

    // Handle the case in which the queue is now empty
    if (empty_rand_q) {
      auto cur_num_queues_o = q_ptrs.num_queues.get(rw, &q_ptrs);
      if (!cur_num_queues_o || !cur_num_queues_o.value()) return {}; // ABORT! // todo: can it be 0 for cur_num_queues_o.value()..?
      uint8_t cur_num_queues = cur_num_queues_o.value();
      
      // Check if we need to swap
      if (rand_queue_idx < (cur_num_queues - 1)) {
        // Swap queue at last slot to here
        if (!swap(rw, rand_queue_idx, cur_num_queues)) return {}; // ABORT!
      }

      // Read open_lanes
      auto cur_open_lanes_o = q_ptrs.open_lanes.get(rw, &q_ptrs);
      if (!cur_open_lanes_o) return {}; // ABORT!
      auto cur_open_lanes = cur_open_lanes_o.value();

      if (cur_open_lanes > 1) {
        // Decrement open_lanes
        if (!q_ptrs.open_lanes.set(rw, &q_ptrs, cur_open_lanes - 1)) return {}; // ABORT!
      }
      // Decrement num_queues
      if (!q_ptrs.num_queues.set(rw, &q_ptrs, cur_num_queues - 1)) return {}; // ABORT!

      // Check if ALL queues are empty
      empty_q = ((cur_num_queues - 1) == 0);
    }
    return p_job;
  }

  bool swap(RWTX &rw, int swap_idx, int n_queues) {
    auto last_q_ptr_o = q_ptrs.q_ptr[n_queues - 1].get(rw, &q_ptrs);
    if (!last_q_ptr_o) return false; // ABORT!
    auto last_q_ptr = last_q_ptr_o.value();

    auto swap_q_ptr_o = q_ptrs.q_ptr[swap_idx].get(rw, &q_ptrs);
    if (!swap_q_ptr_o) return false; // ABORT!
    auto swap_q_ptr = swap_q_ptr_o.value();

    return q_ptrs.q_ptr[n_queues - 1].set(rw, &q_ptrs, swap_q_ptr) &&
           q_ptrs.q_ptr[swap_idx].set(rw, &q_ptrs, last_q_ptr);
  }

  // SINGLE threaded only
  void print_queues(RWTX &rw) {
    auto cur_num_queues = q_ptrs.num_queues.get(rw, &q_ptrs).value();

    for (int i = 0; i < cur_num_queues; i++) {
      auto cur_q = q_ptrs.q_ptr[i].get(rw, &q_ptrs).value();
      cur_q->dump();
    }
  }

  /// Report if the queue is empty
  ///
  /// @param rw The current active transaction
  ///
  /// @return True if it's empty, false if not, NONE on abort
  std::optional<bool> empty(RWTX &rw) {
    auto num_queues_o = q_ptrs.num_queues.get(rw, &q_ptrs);
    if (!num_queues_o) return {}; // ABORT!
    return (num_queues_o.value() == 0);
  }

  template <typename TX> std::pair<long,long> dump(TX &ro, bool print=false) {
    long num_elems = 0;
    long key_sum = 0;
    if (print) std::cout << "[[ \n";
    for (int i = 0; i < MAX_QUEUES; i++) {
      if (print) std::cout << "[QUEUE-" << i << "]: ";
      auto q_ptr = q_ptrs.q_ptr[i].get(ro, &(q_ptrs)).value();
      std::pair<long,long> ret = q_ptr->dump(ro, print);
      num_elems += ret.first;
      key_sum += ret.second;
      //std::cout << "ret.first (num_elems): " << ret.first << ", ret.second (key_sum) " << ret.second << "\n";
    }
    if (print) std::cout << "]]\n";
    return std::make_pair(num_elems, key_sum);
  }

  long dump_ht() {
    long num_elem = 0;
    for (int i = 0; i < MAX_QUEUES; i++) {
      auto q_ptr = q_ptrs.q_ptr[i].get_unsafe();
      num_elem += q_ptr->dump_ht();
    }
    return num_elem;
  }
};
