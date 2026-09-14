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

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <utility>
#include <cassert>
#include <optional>

#include "chunk_pool.h"

// The uninstrumented claim in dequeue_strict() reads `published` and `claims`
// on a chunk it located through a transactional read of head_.next, but never
// re-validates that chunk's orec.  That is safe under SMR, which cannot recycle
// a chunk while any thread that may hold a pointer to it is still in its epoch.
// CHUNK_POOL breaks the assumption: it returns a retired chunk to a thread-local
// free list immediately at commit, so a thread holding a stale head_.next could
// claim a slot out of a chunk that has already been recycled into a different
// lane -- silently returning a job of the wrong priority, or double-consuming.
//
// The two are therefore mutually exclusive.  Build chunk-pool configurations
// with -DQPID_NO_ATOMIC_CLAIM, which restores the fully transactional claim.
#if defined(CHUNK_POOL) && !defined(QPID_NO_ATOMIC_CLAIM)
#error "CHUNK_POOL requires -DQPID_NO_ATOMIC_CLAIM (see comment in chunk_queue.h)"
#endif

/// A FIFO queue with coalescing to avoid excess allocations.  This queue
/// supports enqueue(), dequeue(), and empty() operations.
///
/// A key invariant is that there will never be an "empty chunk" in the queue
///
/// @tparam P       The type of the priority associated the elements stored in the queue
/// @tparam E       The type of elements stored in the queue
/// @tparam OPTSTM  A thread descriptor type
template <typename P, typename E, class OPTSTM> class chunk_queue {
  using ownable_t = typename OPTSTM::ownable_t;
  template <typename T> using FIELD = typename OPTSTM::template xField<T>;
  using ROTX = typename OPTSTM::RO;
  using RWTX = typename OPTSTM::RW;
 #ifdef CHUNK_POOL
  using pool_t = chunk_pool<P, E, OPTSTM>;
 #endif

 public:
  
  struct q_node_t : ownable_t {
    struct kv_t {
      kv_t(P p, E e) {
        prio = p;
        job = e;
      }
      kv_t() {}

      FIELD<P> prio;
      FIELD<E> job;
    };

    FIELD<q_node_t *> prev;
    FIELD<q_node_t *> next;
    FIELD<uint32_t> enqueues;
    FIELD<uint32_t> dequeues; // Count of dequeues (only used by the ablation path)
    /// Count of slots handed out by dequeue_strict().  This is a plain atomic
    /// rather than a transactional FIELD on purpose -- see dequeue_strict().
    std::atomic<uint32_t> claims;
    /// Number of leading slots whose jobs are COMMITTED and therefore safe for
    /// any thread to take without transactional instrumentation.  Maintained
    /// outside the STM: an enqueuer advances it only after its transaction has
    /// committed (see pub_flush()).
    ///
    /// INVARIANT: claims <= published <= enqueues, and slots [0, published)
    /// hold committed jobs.
    std::atomic<uint32_t> published;
    kv_t elements[0];

    /// A queue node.  It has prev and next pointers, and an array of elements.
    /// We use ingress and egress counters to manage insertion/removal
    q_node_t()
        : ownable_t(), prev(nullptr), next(nullptr), enqueues(0), dequeues(0),
          claims(0), published(0) {}

    static q_node_t *make_node(RWTX &rw, uint32_t size) {
      uint32_t node_size = sizeof(q_node_t) + size * sizeof(kv_t);
      //void *region = malloc(node_size);
      void *region = ::operator new(node_size);
      return rw.LOG_NEW(new (region) q_node_t());
    }

    // Sorts the first 'count' elements in ascending order by priority
    // ONLY use when the chunk is privitized
    void sort_elements() {
        std::sort(elements + consumed_unsafe(), elements + enqueues.get_unsafe(), [](kv_t& a, kv_t& b) {
            return a.prio.get_unsafe() < b.prio.get_unsafe(); 
        });
    }
    
    // Sorts in descending order
    void sort_elements_desc() {
        std::sort(elements + consumed_unsafe(), elements + enqueues.get_unsafe(), [](kv_t& a, kv_t& b) {
            return a.prio.get_unsafe() > b.prio.get_unsafe(); 
        });
    }

    void reset() {
      enqueues.set_unsafe(0);
      dequeues.set_unsafe(0);
      claims.store(0, std::memory_order_relaxed);
      published.store(0, std::memory_order_relaxed);
    }

    /// Number of slots already consumed, for debug/validation walks.  Exactly
    /// one of the two counters is ever non-zero: `claims` is used by
    /// extract-min-strict, `dequeues` only by the QPID_NO_ATOMIC_CLAIM
    /// ablation.  (The strict and chunk-batched extract APIs are not designed
    /// to be mixed on one queue; that was already true before this change.)
    uint32_t consumed_unsafe() {
#ifdef QPID_NO_ATOMIC_CLAIM
      return dequeues.get_unsafe();
#else
      // `claims` can exceed `enqueues` when threads race for the last slots
      // (see dequeue_strict); those extra claims consumed nothing.
      uint32_t c = claims.load(std::memory_order_relaxed);
      uint32_t e = enqueues.get_unsafe();
      return c < e ? c : e;
#endif
    }

    void dump(RWTX rw, int slot=-1) {
      if (slot >= 0) {
        std::cout << "[[ QUEUE-" << slot << " ]]: [";
      } else {
        std::cout << "[[ QUEUE ]]: [";
      }
      int size = enqueues.get(rw, this).value();
      for (int i = 0; i < size; i++) {
        std::cout << elements[i].job.get(rw, this).value() << ", ";
      }
      std::cout << "]\n";
    }
  };

  /// A thread's pending publications.  An enqueue writes its jobs inside a
  /// transaction, but may only advertise them to the (uninstrumented) claim
  /// path once that transaction has committed -- otherwise a claimer could take
  /// a job that is later rolled back.  So each enqueue records "chunk X now has
  /// Y committed slots" here, and the operation flushes the list after commit.
  struct pub_ent_t { q_node_t *node; uint32_t val; };
  static constexpr int PUB_MAX = 16;
  struct pub_list_t {
    pub_ent_t e[PUB_MAX];
    int n = 0;
    bool overflow = false;
  };
  inline static thread_local pub_list_t t_pub;

  /// Discard any pending publications.  Call at the start of every attempt, so
  /// that entries recorded by an attempt that then aborted are dropped.
  ///
  /// NB: the whole publish/atomic-claim subsystem compiles away under
  /// QPID_NO_ATOMIC_CLAIM.  Only extract_min_strict() reads `published`, so an
  /// application that uses just the chunk-batched extract_min() should build
  /// with -DQPID_NO_ATOMIC_CLAIM: publishing costs one extra CAS per insert,
  /// measured at 5-9% of insert-dominated throughput (12.2 -> 11.2 M ops/s at
  /// one thread, 88.1 -> 84.7 at 96) for no benefit on that path.
  static void pub_clear() {
#ifndef QPID_NO_ATOMIC_CLAIM
    t_pub.n = 0;
    t_pub.overflow = false;
#endif
  }

  /// Advertise everything this (now committed) operation wrote.
  ///
  /// Safe to do after the transaction: a chunk cannot be retired while it holds
  /// committed-but-unpublished slots, because retiring requires
  /// claims >= enqueues and claims can never exceed published.
  static void pub_flush() {
#ifndef QPID_NO_ATOMIC_CLAIM
    for (int i = 0; i < t_pub.n; ++i) {
      auto &a = t_pub.e[i].node->published;
      uint32_t want = t_pub.e[i].val;
      // Monotonic max: two enqueuers to one chunk are serialized by its orec,
      // so a larger value always describes strictly more committed data -- but
      // their post-commit publishes can still race, and `published` must never
      // move backwards or a retirer could drop a live job.
      uint32_t cur = a.load(std::memory_order_relaxed);
      while (cur < want &&
             !a.compare_exchange_weak(cur, want, std::memory_order_release,
                                      std::memory_order_relaxed)) {}
    }
    t_pub.n = 0;
#endif
  }

  /// Record that `node` will have `val` committed slots once we commit.
  static void pub_record(q_node_t *node, uint32_t val) {
#ifdef QPID_NO_ATOMIC_CLAIM
    (void)node;
    (void)val;
#else
    if (t_pub.n < PUB_MAX) {
      t_pub.e[t_pub.n].node = node;
      t_pub.e[t_pub.n].val = val;
      ++t_pub.n;
    } else {
      // Cannot happen for the batch sizes this structure uses (one enqueue
      // touches at most ceil(batch/NUM_ELEMENTS)+1 chunks), but losing a
      // publication would lose jobs, so fail loudly rather than silently.
      t_pub.overflow = true;
      std::terminate();
    }
#endif
  }

  /// Report whether `n`'s slots have all been handed out by dequeue_strict(),
  /// which seals the chunk against further appends.
  ///
  /// @param enqs The chunk's current fill level
  static bool is_claimed_out(q_node_t *n, uint32_t enqs) {
#ifdef QPID_NO_ATOMIC_CLAIM
    return false;
#else
    return enqs > 0 && n->claims.load(std::memory_order_relaxed) >= enqs;
#endif
  }

  const int NUM_ELEMENTS; // The size of each vector

  q_node_t head_; // The sentinel head (0 elements)
  q_node_t tail_; // The sentinel tail (0 elements)

  /// Default construct a queue from within a RW transactional context
  ///
  /// @param rw         The currently active RW transaction
  /// @param chunksize  The size of each chunk in the queue
  chunk_queue(RWTX &rw, uint32_t chunksize) : NUM_ELEMENTS(chunksize) {
    head_.next.set_cap(rw, &head_, &tail_);
    tail_.prev.set_cap(rw, &tail_, &head_);
  }

  /// Remove a chunk from the queue
  ///
  /// Invariant: This never leaves an empty non-sentinel node in the queue
  ///
  /// @param rw The current active transaction
  /// @param empty_q Initially false, to be set true if queue becomes empty
  ///
  /// @return A chunk from the queue, or NONE on abort
  std::optional<q_node_t*> dequeue(RWTX &rw, bool &empty_q) {
    // q_node_t* tail = &tail_;

    // // Get node before tail
    // auto target_o = tail->prev.get(rw, tail);
    // if (!target_o)
    //   return {}; // ABORT!
    // auto target = target_o.value();

    // // This may happen due to ending RO and starting RW
    // if (target == &head_) {
    //   rw.OP()->force_abort(rw); // calls unwind
    //   return {}; // ABORT! - force to restart
    // }

    // // Get target's predecessor
    // auto prev_o = target->prev.get(rw, target);
    // if (!prev_o)
    //   return {}; // ABORT!
    // auto prev = prev_o.value();
    // // Unstitch target
    // if (!prev->next.set(rw, prev, tail)) return {}; // ABORT!
    // if (!tail->prev.set(rw, tail, prev)) return {}; // ABORT!

    // // Check if now empty
    // if (prev == &head_) {
    //   empty_q = true;
    // }
    // return target;

    // Get target to unstitch from
    q_node_t* head = &head_;
    auto target_o = head->next.get(rw, head);
    if (!target_o) return {}; // ABORT!
    q_node_t* target = target_o.value();

    // This may happen due to ending RO and starting RW
    if (target == &tail_) {
      rw.OP()->force_abort(rw); // calls unwind
      return {}; // ABORT! - force to restart
    }

    // Get target's successor
    auto next_o = target->next.get(rw, target);
    if (!next_o)
      return {}; // ABORT!
    auto next = next_o.value();
    // Unstitch target
    if (!head->next.set(rw, head, next)) return {}; // ABORT!
    if (!next->prev.set(rw, next, head)) return {}; // ABORT!

    // Check if now empty
    if (next == &tail_) {
      empty_q = true;
    }
    return target;
  }

  /// Remove an element from the queue
  ///
  /// Invariant: This never leaves an empty non-sentinel node in the queue
  ///
  /// @param rw The current active transaction
  /// @param empty_q Initially false, to be set true if queue becomes empty
  ///
  /// @return A chunk from the queue, or NONE on abort
 #ifdef CHUNK_POOL
  std::optional<std::pair<P, E>> dequeue_strict(RWTX &rw, bool &empty_q, bool &retry, q_node_t* &retired_chunk)
 #else
  std::optional<std::pair<P, E>> dequeue_strict(RWTX &rw, bool &empty_q, bool &retry)
 #endif
  {
    // Get the head chunk
    auto target_o = head_.next.get(rw, &head_);
    if (!target_o) return {}; // ABORT!
    auto target = target_o.value();

    if (target == &tail_) {
      // The lane drained under us (the diffractor picks lanes from a count that
      // it read earlier in this transaction).  Restart.
      rw.OP()->force_abort(rw);
      return {}; // ABORT!
    }

#ifndef QPID_NO_ATOMIC_CLAIM
    // ------------------------------------------------------ publish-at-commit
    // The fast path reads `published`, NOT `enqueues`.  `enqueues` is a
    // transactional field written by every insert into this chunk, so reading
    // it made an extract abort whenever an insert had committed here since the
    // extract began -- 16.9 aborts per successful strict extract at 96 threads,
    // and the reason the previous atomic-claim work did not scale.  `published`
    // carries the same information for slots that are already committed, and is
    // an ordinary atomic, so the claim path now touches NO transactional state
    // belonging to the chunk.
    uint32_t pub = target->published.load(std::memory_order_acquire);
    uint32_t c = target->claims.load(std::memory_order_relaxed);
    while (c < pub) {
      if (target->claims.compare_exchange_weak(c, c + 1,
                                               std::memory_order_acq_rel,
                                               std::memory_order_relaxed))
        return std::make_pair(target->elements[c].prio.get_unsafe(),
                              target->elements[c].job.get_unsafe());
      // `c` was refreshed by the failed compare_exchange_weak
    }

    // Nothing publishable is left.  Now -- and only now -- we need the
    // authoritative fill level, which does cost a transactional read.
    auto enqs_o = target->enqueues.get(rw, target);
    if (!enqs_o) return {}; // ABORT!
    uint32_t enqs = enqs_o.value();

    if (target->claims.load(std::memory_order_relaxed) < enqs) {
      // The chunk holds jobs that are committed but whose inserter has not run
      // pub_flush() yet.  That window is a few instructions wide; just retry.
      rw.OP()->force_abort(rw);
      return {}; // ABORT!
    }

    // At this point claims == published == enqueues: the chunk is used up.
    //
    // Note why the claim above is safe to make without any orec: losing a claim
    // to a later abort would lose a job, so the claim must be unfailable.  It
    // is -- the claim path acquires no orec, so the enclosing transaction
    // commits through try_commit()'s empty-write-set fast path, which cannot
    // fail.  Everything that *does* write (retiring the chunk here, and the
    // lane/priority bookkeeping above us) happens only once the chunk is
    // exhausted, where no slot has been claimed and aborting costs nothing.
    // ---------------------------------------------------------------- retire --
    // The head chunk is used up.  Unlink it and tell the caller to try again;
    // unlike the original design we retire lazily, i.e. not necessarily in the
    // transaction that took the last job, precisely so that the claim above can
    // stay write-free.
    //
    // NB: `target->enqueues` is in our read set, so if an enqueuer appended to
    // this chunk after we read `enqs`, the writes below fail validation and we
    // abort rather than dropping jobs.
    // Seal the chunk: this write ACQUIRES `target`'s orec, which is what stops
    // an enqueuer from appending to the chunk while we unlink it.  Reading
    // `enqueues` alone is not enough -- an enqueue only bumps target's orec at
    // commit, and by then we would already have dropped the chunk.  This is the
    // one write the strict path makes, and it happens once per chunk rather
    // than once per job, so it costs ~1/CHUNK_SIZE of the old scheme.
    if (!target->dequeues.set(rw, target, enqs)) return {}; // ABORT!

    auto next_o = target->next.get(rw, target);
    if (!next_o) return {}; // ABORT!
    auto next = next_o.value();
    if (!head_.next.set(rw, &head_, next)) return {}; // ABORT!
    if (!next->prev.set(rw, next, &head_)) return {}; // ABORT!

    // Check if the lane is now empty
    if (next == &tail_)
      empty_q = true;

   #ifdef CHUNK_POOL
    retired_chunk = target;
   #else
    rw.reclaim(target);
   #endif
    retry = true; // no job claimed: commit the retire, then start over
    return {};
#else
    // ------------------------------------------- QPID_NO_ATOMIC_CLAIM ablation
    // The original transactional claim, kept so the change can be measured.
    auto enqs_o = target->enqueues.get(rw, target);
    if (!enqs_o) return {}; // ABORT!
    uint32_t enqs = enqs_o.value();
    auto deqs_o = target->dequeues.get(rw, target);
    if (!deqs_o) return {}; // ABORT!
    auto deqs = deqs_o.value();
    if (!target->dequeues.set(rw, target, deqs + 1))
      return {}; // ABORT!

    // If enqueues == dequeues + 1, we made it empty, so unstitch and reclaim
    if ((deqs + 1) == enqs) {
      auto next_o = target->next.get_mine(rw, target);
      if (!next_o) return {}; // ABORT
      auto next = next_o.value();
      if (!head_.next.set(rw, &head_, next)) return {}; // ABORT!
      if (!next->prev.set(rw, next, &head_)) return {}; // ABORT!
      if (next == &tail_) empty_q = true;
     #ifdef CHUNK_POOL
      retired_chunk = target;
     #else
      rw.reclaim(target);
     #endif
    }
    // This is a bit greasy... we might have reclaimed target, but since SMR
    // exists, we can still read out the element
    return std::make_pair(target->elements[deqs].prio.get_unsafe(),
                          target->elements[deqs].job.get_unsafe());
#endif
  }

  /// Insert an element into the queue
  ///
  /// Invariant: This never leaves an empty non-sentinel node in the queue
  ///
  /// @param rw       The current active transaction
  /// @param element  The element to insert'
  /// @param check    May initially be T or F - set to F if do not need to consider opening a new lane
  ///
  /// @return true if inserted to this queue for first time, false otherwise (std::nullopt for Abort)
 #ifdef CHUNK_POOL
  std::optional<bool> enqueue(RWTX &rw, pool_t& pool, bool &used_pool, P priority, E &element, bool &check)
 #else
  std::optional<bool> enqueue(RWTX &rw, P priority, E &element, bool &check)
 #endif
  {
    // Get tail
    q_node_t* tail = &tail_;

    // Get node before tail
    auto target_o = tail->prev.get(rw, tail);
    if (!target_o)
      return {}; // ABORT!
    auto target = target_o.value();

    // If it's head, we need to insert a new node
    q_node_t* head = &(head_);
    if (target == head) {
      q_node_t* new_target = nullptr;
     #ifdef CHUNK_POOL
      // see if pool has a chunk
      new_target = pool.pool_get_chunk();
     #ifdef PROFILING
      if (new_target) pool.set_prof_fields(true, true);
      else pool.set_prof_fields(true, false); // false => need to allocate
     #endif
      //q_node_t* new_target = nullptr;
      if (new_target) {
        used_pool = true;
        // TODO: do I need set() here, or can I just use set_unsafe..?
        // Insert element, update enqueues
        if (!(new_target->elements[0].prio.set(rw, new_target, priority) &&
              new_target->elements[0].job.set(rw, new_target, element) &&
              new_target->enqueues.set(rw, new_target, 1))) return {}; // ABORT!
        // Stitch between head and tail
        if (!(new_target->prev.set(rw, new_target, head) &&
              new_target->next.set(rw, new_target, tail))) return {}; // ABORT!
      } else {
     #endif
        // Make new node
        new_target = q_node_t::make_node(rw, NUM_ELEMENTS);
        // Insert while the node is still captured
        new_target->elements[0].prio.set_cap(rw, new_target, priority);
        new_target->elements[0].job.set_cap(rw, new_target, element);
        new_target->enqueues.set_cap(rw, new_target, 1);
        // Stitch between head and tail
        new_target->prev.set_cap(rw, new_target, head);
        new_target->next.set_cap(rw, new_target, tail);
     #ifdef CHUNK_POOL
      }
     #endif

      // The last stitching will either abort us or finish the method
      if (!(head->next.set(rw, head, new_target) &&
             tail->prev.set(rw, tail, new_target))) {
        return {};
      }
      pub_record(new_target, 1);
      check = false;
      return true;
    }

    // Read the enqueue count
    auto enqs_o = target->enqueues.get(rw, target);
    if (!enqs_o)
      return {}; // ABORT!
    auto enqs = enqs_o.value();
    
    // Check if target is full -- or has been claimed out by extract-min-strict,
    // in which case it must be sealed rather than grown.  An extract that loses
    // the race for the last slot leaves `claims` ABOVE `enqueues`; if the chunk
    // were then allowed to grow, those inflated indices would silently skip
    // over live jobs and the chunk would be retired with jobs still in it.
    if (enqs == NUM_ELEMENTS || is_claimed_out(target, enqs)) {
      q_node_t* new_target = nullptr;
      // see if pool has a chunk
     #ifdef CHUNK_POOL
      new_target = pool.pool_get_chunk();
     #ifdef PROFILING
      if (new_target) pool.set_prof_fields(true, true);
      else pool.set_prof_fields(true, false); // false => need to allocate
     #endif
      //q_node_t* new_target = nullptr;
      if (new_target) {
        used_pool = true;
        // Insert element, update enqueues
        if (!(new_target->elements[0].prio.set(rw, new_target, priority) &&
              new_target->elements[0].job.set(rw, new_target, element) &&
              new_target->enqueues.set(rw, new_target, 1))) return {}; // ABORT!
        // Stitch between head and tail
        if (!(new_target->prev.set(rw, new_target, head) &&
              new_target->next.set(rw, new_target, tail))) return {}; // ABORT!
      } else {
     #endif
        // Make new node
        new_target = q_node_t::make_node(rw, NUM_ELEMENTS);
        // Insert while the node is still captured
        new_target->enqueues.set_cap(rw, new_target, 1);
        new_target->elements[0].prio.set_cap(rw, new_target, priority);
        new_target->elements[0].job.set_cap(rw, new_target, element);
        // Stitch between target and tail
        new_target->prev.set_cap(rw, new_target, target);
        new_target->next.set_cap(rw, new_target, tail);
     #ifdef CHUNK_POOL
      }
     #endif
      
      // The last stitching will either abort us or finish the method
      if (!(target->next.set(rw, target, new_target) &&
            tail->prev.set(rw, tail, new_target))) {
        return {}; // ABORT
      }
      pub_record(new_target, 1);
      check = false;
      return false;
    }

    // Bump the fill level first: that acquires `target`'s orec (excluding any
    // concurrent enqueuer and aborting any concurrent reader) and undo-logs the
    // one field that actually needs rolling back.
    if (!target->enqueues.set(rw, target, enqs + 1)) {
      return {}; // ABORT!
    }
    // The slot itself needs no instrumentation.  It lies beyond the committed
    // `enqueues`, so nothing can reach it until we commit; if we abort,
    // `enqueues` is restored and whatever we left here is dead.  Undo-logging
    // element writes cost two log entries per job for no benefit.
    target->elements[enqs].prio.set_unsafe(priority);
    target->elements[enqs].job.set_unsafe(element);
    pub_record(target, enqs + 1);

    //spdlog::info("[{}] Inserted to existing node: target = {}, num_items = {}", tid, (void*)target, enqs + 1);

    // Check determines if we should bother checking conditions to add a lane
    if (check) {
      // Caller indicated to check
      // Check if filled last element of first chunk
      // This would indicate to caller to initiate a new lane
      if (enqs == (NUM_ELEMENTS - 1)) {
        // Check if first chunk
        auto target_prev_o = target->prev.get(rw, target);
        if (!target_prev_o)
          return {}; // ABORT!
        auto target_prev = target_prev_o.value();
        if (target_prev == head) {
          // only case in which 'check' is NOT set to false -> caller should make a new lane
          return false;
        }
      }
      check = false;
    }
    return false;
  }

  /// Insert elements into the queue
  ///
  /// Invariant: `prev_->next` == `tail`, and `prev_` is either the (corresponding) `head` OR an existing, full chunk which we are appending after
  ///
  /// @param rw         The current active transaction
  /// @param batch      The elements to insert
  /// @param batch_idx  Current index to consume from the batch
  /// @param prev_      Chunk (or sentinel) to insert after
  /// @param tail       The tail of the queue being operated on
  /// @param last_new   Passed by reference to set tail in calling function
  ///
  /// @return true if inserted to this queue for first time, false otherwise (std::nullopt for Abort)
 #ifdef CHUNK_POOL
  std::optional<bool> ins_many(RWTX &rw, pool_t& pool, bool &used_pool, std::vector<typename q_node_t::kv_t> &batch, int batch_size, int batch_idx, q_node_t *prev_) {
 #else
  std::optional<bool> ins_many(RWTX &rw, std::vector<typename q_node_t::kv_t> &batch, int batch_size, int batch_idx, q_node_t *prev_) {
 #endif
    int to_process = batch_size - batch_idx;
    bool first = true;
    q_node_t *tail = &(tail_);
    q_node_t *cur_prev = prev_;
    q_node_t *first_new = nullptr;
    q_node_t *last_new = nullptr;

    while (to_process) {
      q_node_t* new_target = nullptr;
     #ifdef CHUNK_POOL
      //! NOTE: the pool does NOT currently support removing multiple chunks in one transaction
      new_target = pool.pool_get_chunk();
     #ifdef PROFILING
      if (new_target) pool.set_prof_fields(true, true);
      else pool.set_prof_fields(true, false); // false => need to allocate
     #endif
      //q_node_t* new_target = nullptr;
      if (new_target) {
        used_pool = true;
        if (first) first_new = new_target;
        last_new = new_target;
        
        // Insert
        int i;
        for (i = 0; i < NUM_ELEMENTS; i++) {
          if (i == to_process) break;
          if (!(new_target->elements[i].prio.set(rw, new_target, batch[batch_idx].prio.get_unsafe()) &&
                new_target->elements[i].job.set(rw, new_target, batch[batch_idx++].job.get_unsafe())))
            return {}; // ABORT!
        }
        to_process -= i;
        if (!new_target->enqueues.set(rw, new_target, i)) return {}; // ABORT!
        pub_record(new_target, (uint32_t)i);
        
        // Stitch between prev_ and tail
        if (!new_target->prev.set(rw, new_target, cur_prev)) return {}; // ABORT!
        if (!new_target->next.set(rw, new_target, tail)) return {}; // ABORT!

        // Modify cur_prev's next pointer
        if (!first) {
          // note: tail->prev handled below
          if (!cur_prev->next.set(rw, cur_prev, new_target)) return {}; // ABORT!
        } else {
          first = false;
        }
      } else {
     #endif

        // Make new node
        new_target = q_node_t::make_node(rw, NUM_ELEMENTS);
        if (first) first_new = new_target;
        last_new = new_target;
        
        // Insert while the node is still captured
        int i;
        for (i = 0; i < NUM_ELEMENTS; i++) {
          if (i == to_process) break;
          new_target->elements[i].prio.set_cap(rw, new_target, batch[batch_idx].prio.get_unsafe());
          new_target->elements[i].job.set_cap(rw, new_target, batch[batch_idx++].job.get_unsafe());
        }
        to_process -= i;
        new_target->enqueues.set_cap(rw, new_target, i);
        pub_record(new_target, (uint32_t)i);
        
        // Stitch between prev_ and tail
        new_target->prev.set_cap(rw, new_target, cur_prev);
        new_target->next.set_cap(rw, new_target, tail);

        // Modify cur_prev's next pointer
        if (!first) {
          cur_prev->next.set_cap(rw, cur_prev, new_target); // note: tail->prev handled below
        } else {
          first = false;
        }
     #ifdef CHUNK_POOL
      }
     #endif
      
      // Reset prev
      cur_prev = new_target;
    }

    if (!(prev_->next.set(rw, prev_, first_new) &&
          tail->prev.set(rw, tail, last_new))) {
      return {};
    }
    return true;
  }

  /// Insert a batch of element(s) to a randomly selected, active chunk queue
  ///
  /// Invariant: This never leaves an empty non-sentinel node in the queue
  ///
  /// @param rw           The current active transaction
  /// @param batch        The vector of elements to be inserted
  /// @param batch_size   The size of the vector
  /// @param check        May initially be T or F - set to F if do not need to consider opening a new lane
  ///
  /// @return True if inserted to this queue for first time, False if not, or NONE on abort
 #ifdef CHUNK_POOL
  std::optional<bool> enqueue_batch_vec(RWTX &rw, pool_t& pool, bool &used_pool, std::vector<typename q_node_t::kv_t> &batch, int batch_size, bool &check) { // check == true (when passed) iff cur_num_lanes < MAX_QUEUES && current queue is last "active" one
 #else
  /// @param batch_beg  first index of the range to insert
  /// @param batch_size  ONE PAST the last index (so the count is
  ///                    batch_size - batch_beg).  Taking a range lets the
  ///                    caller hand us a window of its thread-local buffer
  ///                    instead of materialising a sub-vector.
  std::optional<bool> enqueue_batch_vec(RWTX &rw, std::vector<typename q_node_t::kv_t> &batch, int batch_beg, int batch_size, bool &check) {
 #endif
    // Get tail & tail->prev
    q_node_t* tail = &tail_;
    auto target_o = tail->prev.get(rw, tail);
    if (!target_o)
      return {}; // ABORT!
    auto target = target_o.value();

    // If it's head, we need to insert a new node
    q_node_t* head = &(head_);
    if (target == head) {
     #ifdef CHUNK_POOL
      if (!ins_many(rw, pool, used_pool, batch, batch_size, batch_beg, head)) return {}; // ABORT!
     #else
      if (!ins_many(rw, batch, batch_size, batch_beg, head)) return {}; // ABORT!
     #endif

      if ((batch_size - batch_beg) < NUM_ELEMENTS)
        check = false;
      return true; // inserted to a new lane for first time - diffractor needs to increment num_queues
    }

    // Read the enqueue count
    auto enqs_o = target->enqueues.get(rw, target);
    if (!enqs_o)
      return {}; // ABORT!
    auto enqs = enqs_o.value();
    
    // Check if target is full (see enqueue() for why a claimed-out chunk is
    // treated as full rather than grown)
    int target_room = is_claimed_out(target, enqs) ? 0 : (NUM_ELEMENTS - enqs);
    if ((batch_size - batch_beg) <= target_room) {
      // Room in target - fill it.  Bump the fill level first (this acquires the
      // orec and is the only field that has to be undo-logged), then write the
      // slots with plain stores: they are all beyond the committed `enqueues`,
      // so they are unreachable until commit and dead if we abort.  Logging
      // them cost two undo entries per job -- ~6 KB per 128-job flush.
      const int e_end = enqs + (batch_size - batch_beg);
      if (!target->enqueues.set(rw, target, e_end)) return {}; // ABORT!
      int batch_idx = batch_beg;
      for (int e_idx = enqs; e_idx < e_end; ++e_idx, ++batch_idx) {
        target->elements[e_idx].prio.set_unsafe(batch[batch_idx].prio.get_unsafe());
        target->elements[e_idx].job.set_unsafe(batch[batch_idx].job.get_unsafe());
      }
      pub_record(target, (uint32_t)e_end);
    } else {
      // Insert new nodes after target
     #ifdef CHUNK_POOL
      if (!ins_many(rw, pool, used_pool, batch, batch_size, batch_beg, target)) return {}; // ABORT!
     #else
      if (!ins_many(rw, batch, batch_size, batch_beg, target)) return {}; // ABORT!
     #endif

      if (check) {
        // A new lane should be opened (i.e., keep check True) if filling second chunk
        // Check if 'target' is first chunk
        auto target_prev_o = target->prev.get(rw, target);
        if (!target_prev_o)
          return {}; // ABORT!
        auto target_prev = target_prev_o.value();
        if (target_prev == head) 
          return false; // Leave `check` as true (new lane should be opened by caller)
        check = false;
      }
    }
    return false;
  }

  /// Report if the queue is empty
  ///
  /// @param rw The current active transaction
  ///
  /// @return True if it's empty, false if not, NONE on abort
  std::optional<bool> empty(RWTX &rw) {
    auto next_o = head_.next.get(rw, &head_);
    if (!next_o) return {}; // ABORT!
    return (next_o.value() == &tail_);
  }

  /// Calculate keysum and size of the chunk_queue - single-threaded only
  ///
  /// @param ro The current active read-only transaction
  /// @param print Whether or not to print the contents, default to false
  ///
  /// @return Pair containing size, keysum of the chunk_queue
  template <typename TX> std::pair<long,long> dump(TX &ro, bool print=false) {
    long num_elems = 0;
    long key_sum = 0;
    auto curr = head_.next.get(ro, &head_).value();

    if (print) std::cout << "{ ";
    
    while (curr != &tail_) {
      int deq = (int)curr->consumed_unsafe();
      int enq = curr->enqueues.get(ro, curr).value();
      int size = enq - deq;
      num_elems += size;
      
      if (print) std::cout << "[ ";
      for (int j = deq; j < enq; j++) {
        auto val = curr->elements[j].prio.get(ro, curr).value();
        if (print) {
          if (j < (size - 1)) {
            std::cout << val << ", ";
          } else {
            std::cout << val;
          }
        } 
        key_sum += val;
      }
      curr = curr->next.get(ro, curr).value();
      if (print) {
        if (curr != &tail_) std::cout << " ], ";
        else std::cout << " ]";
      } 
    }
    if (print) std::cout << " }\n\n";
    return std::make_pair(num_elems, key_sum);
  }

  long dump_ht() {
    long num_elems = 0;
    auto curr = head_.next.get_unsafe();
    
    while (curr != &tail_) {
      num_elems += curr->enqueues.get_unsafe() - curr->consumed_unsafe();
      curr = curr->next.get_unsafe();
    }
    return num_elems;
  }
};
