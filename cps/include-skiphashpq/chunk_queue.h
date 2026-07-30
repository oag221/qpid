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

#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <utility>
#include <cassert>
#include <optional>

#include "chunk_pool.h"

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
    FIELD<uint32_t> dequeues; // Count of dequeues
    kv_t elements[0];

    /// A queue node.  It has prev and next pointers, and an array of elements.
    /// We use ingress and egress counters to manage insertion/removal
    q_node_t()
        : ownable_t(), prev(nullptr), next(nullptr), enqueues(0), dequeues(0) {}

    static q_node_t *make_node(RWTX &rw, uint32_t size) {
      uint32_t node_size = sizeof(q_node_t) + size * sizeof(kv_t);
      //void *region = malloc(node_size);
      void *region = ::operator new(node_size);
      return rw.LOG_NEW(new (region) q_node_t());
    }

    void reset() {
      enqueues.set_unsafe(0);
      dequeues.set_unsafe(0);
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
  std::optional<std::pair<P, E>> dequeue_strict(RWTX &rw, bool &empty_q, q_node_t* &retired_chunk)
 #else
  std::optional<std::pair<P, E>> dequeue_strict(RWTX &rw, bool &empty_q)
 #endif
  {
    // Get target to unstitch from
    auto target_o = head_.next.get(rw, &head_);
    if (!target_o) return {}; // ABORT!
    auto target = target_o.value();

    // Increment dequeue count
    auto deqs_o = target->dequeues.get(rw, target);
    if (!deqs_o)
      return {}; // ABORT!
    auto deqs = deqs_o.value();
    if (!target->dequeues.set(rw, target, deqs + 1))
      return {}; // ABORT!
    
      // Read the enqueue count
    auto enqs_o = target->enqueues.get_mine(rw, target);
    if (!enqs_o)
      return {}; // ABORT!
    auto enqs = enqs_o.value();

    // If enqueues == dequeues + 1, we made it empty, so unstitch and reclaim
    if ((deqs + 1) == enqs) {
      // Get target's successor
      auto next_o = target->next.get_mine(rw, target);
      if (!next_o)
        return {}; // ABORT
      auto next = next_o.value();
      // Unstitch target
      if (!head_.next.set(rw, &head_, next))
        return {}; // ABORT!
      if (!next->prev.set(rw, next, &head_))
        return {}; // ABORT!

      // Check if empty
      if (next == &tail_)
        empty_q = true;
      
     #ifdef CHUNK_POOL
      // Return to be added to pool
      retired_chunk = target;
     #else
      // Reclaim target
      rw.reclaim(target);
     #endif
    }
    // This is a bit greasy... we might have reclaimed target, but since SMR
    // exists, we can still read out the element
    return std::make_pair(target->elements[deqs].prio.get_unsafe(), target->elements[deqs].job.get_unsafe());
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
      check = false;
      return true;
    }

    // Read the enqueue count
    auto enqs_o = target->enqueues.get(rw, target);
    if (!enqs_o)
      return {}; // ABORT!
    auto enqs = enqs_o.value();
    
    // Check if target is full
    if (enqs == NUM_ELEMENTS) {
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
      check = false;
      return false;
    }

    // Update the enqueue count, put the element in
    if (!(target->enqueues.set(rw, target, enqs + 1) &&
          target->elements[enqs].prio.set(rw, target, priority) &&
          target->elements[enqs].job.set(rw, target, element))) {
      return {}; // ABORT!
    }

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
  std::optional<bool> enqueue_batch_vec(RWTX &rw, std::vector<typename q_node_t::kv_t> &batch, int batch_size, bool &check) { // check == true (when passed) iff cur_num_lanes < MAX_QUEUES && current queue is last "active" one
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
      if (!ins_many(rw, pool, used_pool, batch, batch_size, 0, head)) return {}; // ABORT!
     #else
      if (!ins_many(rw, batch, batch_size, 0, head)) return {}; // ABORT!
     #endif
        
      if (batch_size < NUM_ELEMENTS) 
        check = false;
      return true; // inserted to a new lane for first time - diffractor needs to increment num_queues
    }

    // Read the enqueue count
    auto enqs_o = target->enqueues.get(rw, target);
    if (!enqs_o)
      return {}; // ABORT!
    auto enqs = enqs_o.value();
    
    // Check if target is full
    int target_room = NUM_ELEMENTS - enqs;
    if (batch_size <= target_room) {
      // Room in target - fill it
      int batch_idx = 0;
      int e_idx;
      for (e_idx = enqs; e_idx < NUM_ELEMENTS; e_idx++) {
        if (batch_idx == batch_size) break;
        if (batch_idx == 0) {
          if (!(target->elements[e_idx].prio.set(rw, target, batch[batch_idx].prio.get_unsafe()) &&
                target->elements[e_idx].job.set(rw, target, batch[batch_idx++].job.get_unsafe()))) return {}; // ABORT!
        } else {
          target->elements[e_idx].prio.set_mine(rw, target, batch[batch_idx].prio.get_unsafe());
          target->elements[e_idx].job.set_mine(rw, target, batch[batch_idx++].job.get_unsafe());
        }
      }
      // Re-set target's enqs
      if (!target->enqueues.set(rw, target, e_idx)) return {}; // ABORT!
    } else {
      // Insert new nodes after target
     #ifdef CHUNK_POOL
      if (!ins_many(rw, pool, used_pool, batch, batch_size, 0, target)) return {}; // ABORT!
     #else
      if (!ins_many(rw, batch, batch_size, 0, target)) return {}; // ABORT!
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
      int deq = curr->dequeues.get(ro, curr).value();
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
      num_elems += curr->enqueues.get_unsafe() - curr->dequeues.get_unsafe();
      curr = curr->next.get_unsafe();
    }
    return num_elems;
  }
};
