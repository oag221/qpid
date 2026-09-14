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

/// QPID_LANE_SLACK: how many *empty* lanes, beyond the currently non-empty
/// ones, an insert is allowed to choose from.
///
/// Larger values spread concurrent inserts over more lanes, which reduces
/// contention on any one lane; but they also leave each chunk shallower, which
/// costs twice over: more lane empty<->non-empty transitions (each writes the
/// shared per-priority metadata under a single orec) and smaller batches for
/// extract to hand out.  The right value therefore depends on how *deep* a
/// priority bucket is, i.e. on how many jobs share a priority.
///
/// Measured, 50/50 insert/extract, 1M prefill, q=128 c=128, flat keys,
/// throughput in M ops/s (base = the eager-lane code this replaced):
///
///          15 distinct priorities        1000 distinct priorities
///          t=1    t=24   t=48   t=96     t=1   t=24   t=48   t=96
///   base   11.2   88.7   70.2    3.5     6.5   23.5    9.8    0.8
///   =1     11.9  102.9   45.7   61.3     5.3   33.4   15.5   15.8
///   =2     11.9  102.4   64.8   84.4     5.0   35.9   10.3   11.1
///   =4     11.9  102.5   72.8   88.0     4.4   23.5    6.9    7.8
///   =8     11.9   99.6   74.7   78.8     4.1   21.8    6.4    6.9
///
/// 2 is the default because it is the largest value that does not regress the
/// many-priorities regime at scale.  Raise it to 4 when jobs-per-priority is
/// known to be high; drop it to 1 when priorities are numerous and shallow.
/// Making this adapt to observed bucket depth, instead of being a constant, is
/// the obvious next step.
#ifndef QPID_LANE_SLACK
#define QPID_LANE_SLACK 2
#endif

/// QPID_INIT_LANES: how many lanes are constructed when a priority bucket is
/// created.  The remaining lanes are constructed on demand (see grow_to), so
/// bucket creation is O(QPID_INIT_LANES) rather than O(MAX_QUEUES).
#ifndef QPID_INIT_LANES
#define QPID_INIT_LANES 1
#endif

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
    /// The number of non-empty lanes.  INVARIANT: q_ptr[0 .. num_queues) are
    /// exactly the non-empty lanes, and q_ptr[num_queues .. alloc_lanes) are
    /// exactly the constructed-but-empty ones.
    FIELD<uint32_t> num_queues;
    /// How many lanes have actually been constructed.  q_ptr[i] is only valid
    /// for i < alloc_lanes.  Monotonically non-decreasing.
    FIELD<uint32_t> alloc_lanes;
    FIELD<Q_T*> *q_ptr;

    q_ptr_t(RWTX &rw, uint32_t chunksize, int max_queues)
        : ownable_t(), num_queues(0), alloc_lanes(0) {
      // NB: deliberately left uninitialized -- entries are written by grow_to()
      // as lanes are constructed, and never read above alloc_lanes.
      q_ptr = (FIELD<Q_T*>*)malloc(max_queues * sizeof(FIELD<Q_T*>));
    }

    ~q_ptr_t() {
      free(q_ptr);
    }
  };

  const int MAX_QUEUES;
  const uint32_t CHUNK_SIZE; // needed to construct lanes lazily
  inline static thread_local random_num rand_engine;

  Q_T *queues;
  q_ptr_t q_ptrs;

  /// Default construct a queue from within a RW transactional context
  ///
  /// @param rw         The currently active RW transaction
  /// @param chunksize  The size of each chunk in the queue
  chunk_queue_diffractor(RWTX &rw, uint32_t chunksize, uint32_t n_queues) :
        MAX_QUEUES(n_queues),
        CHUNK_SIZE(chunksize),
        q_ptrs(rw, chunksize, n_queues) {
    // The lane array is reserved but *not* touched: only QPID_INIT_LANES lanes
    // are constructed here, the rest are constructed by grow_to() on demand.
    // Constructing all MAX_QUEUES lanes eagerly made creating a priority bucket
    // cost O(MAX_QUEUES) -- ~33k cycles and ~13KB of cold memory at q=128 --
    // inside the inserting transaction, so every abort paid for it again.
    queues = (Q_T *)malloc(n_queues * sizeof(Q_T));
    uint32_t init = QPID_INIT_LANES;
    if (init > n_queues) init = n_queues;
    if (init < 1) init = 1;
    for (uint32_t i = 0; i < init; i++) {
      new (&queues[i]) Q_T(rw, chunksize);
      // `this` is still captured (unreachable by other threads), so no orec.
      q_ptrs.q_ptr[i].set_cap(rw, &q_ptrs, &(queues[i]));
    }
    q_ptrs.alloc_lanes.set_cap(rw, &q_ptrs, init);
  }

private:
  /// Make sure lane `want` exists, constructing lanes on demand with geometric
  /// growth so a bucket pays O(log MAX_QUEUES) growth transactions in total.
  ///
  /// @return A usable (constructed) lane index, or -1 on abort.
  int grow_to(RWTX &rw, uint32_t want) {
    auto al_o = q_ptrs.alloc_lanes.get(rw, &q_ptrs);
    if (!al_o) return -1; // ABORT!
    uint32_t al = al_o.value();
    if (want < al) return (int)want; // already constructed
    if (al >= (uint32_t)MAX_QUEUES) return (int)(al - 1);

    uint32_t nal = al << 1;
    if (nal <= want) nal = want + 1;
    if (nal > (uint32_t)MAX_QUEUES) nal = MAX_QUEUES;

    // IMPORTANT: acquire the orec *before* running any placement-new.  If two
    // threads both construct queues[i] and the loser's construction lands after
    // the winner has committed a chunk into that lane, the chunk is silently
    // lost and num_queues stays > 0 with every lane empty -- which makes
    // extract-min force-abort forever.
    if (!q_ptrs.alloc_lanes.set(rw, &q_ptrs, nal)) return -1; // ABORT!
    for (uint32_t i = al; i < nal; ++i) {
      new (&queues[i]) Q_T(rw, CHUNK_SIZE);
      // We hold the orec courtesy of the set() above.
      q_ptrs.q_ptr[i].set_mine(rw, &q_ptrs, &(queues[i]));
    }
    return (int)(want < nal ? want : nal - 1);
  }

  /// Choose a lane for an insert, spreading over the non-empty lanes plus up to
  /// QPID_LANE_SLACK empty ones, and construct it if necessary.
  ///
  /// @param nq  The current number of non-empty lanes (already read by caller)
  ///
  /// @return A usable lane index, or -1 on abort
  int pick_insert_lane(RWTX &rw, uint32_t nq) {
    uint32_t lim = nq + (uint32_t)QPID_LANE_SLACK;
    if (lim > (uint32_t)MAX_QUEUES) lim = MAX_QUEUES;
    if (lim < 1) lim = 1;
    uint64_t idx = (lim > 1) ? random_number(lim - 1) : 0;
    return grow_to(rw, (uint32_t)idx);
  }

  /// Move a lane that just became non-empty into the non-empty prefix, so that
  /// extract can keep picking uniformly from q_ptr[0 .. num_queues).
  ///
  /// Precondition: lane `idx` was empty (hence idx >= nq) and is constructed
  /// (hence idx < alloc_lanes), so nq < alloc_lanes and q_ptr[nq] is valid.
  bool promote_lane(RWTX &rw, uint32_t idx, uint32_t nq, Q_T *lane) {
    if (idx != nq) {
      auto other_o = q_ptrs.q_ptr[nq].get(rw, &q_ptrs);
      if (!other_o) return false; // ABORT!
      if (!q_ptrs.q_ptr[nq].set(rw, &q_ptrs, lane)) return false;            // ABORT!
      if (!q_ptrs.q_ptr[idx].set(rw, &q_ptrs, other_o.value())) return false; // ABORT!
    }
    return q_ptrs.num_queues.set(rw, &q_ptrs, nq + 1);
  }

public:

  ~chunk_queue_diffractor() {
    free(queues);
  }

  /// Uniform random value in [0, max_k] INCLUSIVE.
  ///
  /// This used to construct a std::uniform_int_distribution on every call,
  /// which does rejection sampling with a division.  It runs on every insert
  /// and every extract, so it is worth the multiply-shift form instead (a
  /// negligible modulo bias is fine -- the result only picks a lane).
  uint64_t random_number(int max_k) {
    const uint32_t n = (uint32_t)max_k + 1u;
    const uint32_t r = (uint32_t)rand_engine();
    return (uint64_t)(((uint64_t)r * (uint64_t)n) >> 32);
  }

  template <typename TX> std::optional<uint64_t> random_queue_idx(TX &tx) {
    // Get current number of queues
    auto cur_num_queues_o = q_ptrs.num_queues.get(tx, &q_ptrs);
    if (!cur_num_queues_o) return {}; // ABORT!
    #ifdef PROFILING
    if (!cur_num_queues_o.value()) {
      std::cout << "NUM QUEUES IS 0\n";
      // Must unwind: a bare `return {}` here leaves the transaction open, the
      // caller retries, and the RwStm destructor then trips its in_tx
      // assertion and calls std::terminate().  astar builds with -DPROFILING,
      // so this path is live there.
      tx.OP()->force_abort(tx);
      return {};
    }
    #endif
    uint32_t cur_num_queues = cur_num_queues_o.value();

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
    // Read the number of non-empty lanes, then pick a lane to insert into from
    // the non-empty ones plus up to QPID_LANE_SLACK empty ones.  The old code
    // picked from a separate `open_lanes` counter that only grew when an insert
    // happened to fill the *first* chunk of the *last* open lane, but shrank on
    // every lane drain; under load it collapsed to one lane and serialized all
    // inserts on a single chunk_queue.
    auto nq_o = q_ptrs.num_queues.get(rw, &q_ptrs);
    if (!nq_o) return false; // ABORT!
    uint32_t nq = nq_o.value();

    int idx_i = pick_insert_lane(rw, nq);
    if (idx_i < 0) return false; // ABORT!
    uint32_t idx = (uint32_t)idx_i;

    // Get the proper queue
    auto queue_o = q_ptrs.q_ptr[idx].get(rw, &q_ptrs);
    if (!queue_o) return false; // ABORT!
    auto queue = queue_o.value();

    bool check = false; // the `open_lanes` mechanism is gone
   #ifdef CHUNK_POOL
    auto enq_ret_o = queue->enqueue(rw, pool, used_pool, prio, element, check);
   #else
    auto enq_ret_o = queue->enqueue(rw, prio, element, check);
   #endif
    if (!enq_ret_o) return false; // ABORT!

    // (enq_ret == true) means the lane was empty and has just become non-empty,
    // so it has to be moved into the non-empty prefix.
    if (enq_ret_o.value() && !promote_lane(rw, idx, nq, queue))
      return false; // ABORT!
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
  std::optional<bool> enqueue_batch_vec(RWTX &rw, pool_t& pool, bool &used_pool, std::vector<typename Q_T::q_node_t::kv_t> &batch, int batch_beg, int batch_size) {
 #else
  std::optional<bool> enqueue_batch_vec(RWTX &rw, std::vector<typename Q_T::q_node_t::kv_t> &batch, int batch_beg, int batch_size) {
 #endif
    // Same lane-selection policy as enqueue(); see the comment there.
    auto nq_o = q_ptrs.num_queues.get(rw, &q_ptrs);
    if (!nq_o) return {}; // ABORT!
    uint32_t nq = nq_o.value();

    int idx_i = pick_insert_lane(rw, nq);
    if (idx_i < 0) return {}; // ABORT!
    uint32_t idx = (uint32_t)idx_i;

    // Get the proper queue
    auto queue_o = q_ptrs.q_ptr[idx].get(rw, &q_ptrs);
    if (!queue_o) return {}; // ABORT!
    auto queue = queue_o.value();

    bool check = false; // the `open_lanes` mechanism is gone
   #ifdef CHUNK_POOL
    auto enq_ret_o = queue->enqueue_batch_vec(rw, pool, used_pool, batch, batch_beg, batch_size, check);
   #else
    auto enq_ret_o = queue->enqueue_batch_vec(rw, batch, batch_beg, batch_size, check);
   #endif
    if (!enq_ret_o) return {}; // ABORT!

    if (enq_ret_o.value() && !promote_lane(rw, idx, nq, queue))
      return {}; // ABORT!
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
      uint32_t cur_num_queues = cur_num_queues_o.value();
      
      // Check if we need to swap
      if (rand_queue_idx < (cur_num_queues - 1)) {
        // Swap queue at last slot to here
        if (!swap(rw, rand_queue_idx, cur_num_queues)) return {}; // ABORT!
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
  std::optional<std::pair<P, E>> dequeue_strict(RWTX &rw, bool &empty_q, bool &retry, typename Q_T::q_node_t* &retired_chunk) {
 #else
  std::optional<std::pair<P, E>> dequeue_strict(RWTX &rw, bool &empty_q, bool &retry) {
 #endif
    // Get a random queue (idx) to extract from
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
    auto ret_o = queue->dequeue_strict(rw, empty_rand_q, retry, retired_chunk);
   #else
    auto ret_o = queue->dequeue_strict(rw, empty_rand_q, retry);
   #endif
    // `retry` means the lane's head chunk was exhausted and has been retired:
    // no job was claimed, but the transaction is still live and its writes
    // (including the bookkeeping below) still need to commit.
    if (!ret_o && !retry) return {}; // ABORT!

    // From here on, any abort must also clear `retry`: the caller uses it to
    // decide whether to commit, and committing an already-unwound transaction
    // trips the STM's in_tx assertion.
    if (empty_rand_q) {
      auto cur_num_queues_o = q_ptrs.num_queues.get(rw, &q_ptrs);
      if (!cur_num_queues_o) { retry = false; return {}; } // ABORT!
      uint32_t cur_num_queues = cur_num_queues_o.value();
      if (!cur_num_queues) {
        // Should be unreachable: the lane we just emptied was counted here.
        rw.OP()->force_abort(rw);
        retry = false;
        return {}; // ABORT!
      }

      // Check if we need to swap
      if (rand_queue_idx < (cur_num_queues - 1)) {
        // Swap queue at last slot to here
        if (!swap(rw, rand_queue_idx, cur_num_queues)) { retry = false; return {}; } // ABORT!
      }

      // Decrement num_queues
      if (!q_ptrs.num_queues.set(rw, &q_ptrs, cur_num_queues - 1)) { retry = false; return {}; } // ABORT!

      // Check if ALL queues are empty
      empty_q = ((cur_num_queues - 1) == 0);
    }
    if (retry) return {}; // caller must commit, then start over
    return ret_o.value();
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
    // Only lanes below alloc_lanes have been constructed.
    uint32_t n_alloc = q_ptrs.alloc_lanes.get(ro, &(q_ptrs)).value();
    for (uint32_t i = 0; i < n_alloc; i++) {
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
    uint32_t n_alloc = q_ptrs.alloc_lanes.get_unsafe();
    for (uint32_t i = 0; i < n_alloc; i++) {
      auto q_ptr = q_ptrs.q_ptr[i].get_unsafe();
      num_elem += q_ptr->dump_ht();
    }
    return num_elem;
  }
};
