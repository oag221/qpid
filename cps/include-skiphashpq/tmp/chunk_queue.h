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

/// A FIFO queue with coalescing to avoid excess allocations.  This queue
/// supports enqueue(), dequeue(), and empty() operations.
///
/// A key invariant is that there will never be an "empty chunk" in the queue
///
/// @tparam P       The priority of elements stored in the queue
/// @tparam E       The type of elements stored in the queue
/// @tparam OPTSTM  A thread descriptor type
template <typename P, typename E, class OPTSTM> class chunk_queue {
  using ownable_t = typename OPTSTM::ownable_t;
  template <typename T> using FIELD = typename OPTSTM::template xField<T>;
  using ROTX = typename OPTSTM::RO;
  using RWTX = typename OPTSTM::RW;

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

    void dump(RWTX rw, int slot=-1) {
      if (slot >= 0) {
        std::cout << "[[ QUEUE-" << slot << " ]]: [";
      } else {
        std::cout << "[[ QUEUE ]]: [";
      }
      int size = enqueues.get(rw, this).value();
      for (int i = 0; i < size; i++) {
        std::cout << elements[i].prio.get(rw, this).value() << ", ";
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

    // // ! End read-only transaction
    // me->end_ro();
    // // ! Start read-write transaction
    // RWTX rw(me);

    // Re-read target - need orec due to starting new transction
    // target_o = head->next.get(rw, head);
    // if (!target_o) return {}; // ABORT!
    // target = target_o.value();

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
    //return std::make_pair(target, std::move(rw));
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
  std::optional<std::pair<P,E>> dequeue_strict(RWTX &rw, bool &empty_q) {
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
      
      // Reclaim target
      rw.reclaim(target);
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
  std::optional<bool> enqueue(RWTX &rw, P &prio, E &job, bool &check) {
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
      // Make new node
      auto new_target = q_node_t::make_node(rw, NUM_ELEMENTS);
      // Insert while the node is still captured
      new_target->elements[0].prio.set_cap(rw, new_target, prio);
      new_target->elements[0].job.set_cap(rw, new_target, job);
      new_target->enqueues.set_cap(rw, new_target, 1);
      // Stitch between head and tail
      new_target->prev.set_cap(rw, new_target, head);
      new_target->next.set_cap(rw, new_target, tail);

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
      // Make new node
      auto new_target = q_node_t::make_node(rw, NUM_ELEMENTS);
      // Insert while the node is still captured
      new_target->enqueues.set_cap(rw, new_target, 1);
      new_target->elements[0].prio.set_cap(rw, new_target, prio);
      new_target->elements[0].job.set_cap(rw, new_target, job);
      // Stitch between target and tail
      new_target->prev.set_cap(rw, new_target, target);
      new_target->next.set_cap(rw, new_target, tail);
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
          target->elements[enqs].prio.set(rw, target, prio) &&
          target->elements[enqs].job.set(rw, target, job))) {
      return {}; // ABORT!
    }

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
  std::optional<bool> ins_many(RWTX &rw, std::vector<std::pair<P,E>> &batch, int batch_size, int batch_idx, q_node_t *prev_) {
    int to_process = batch_size - batch_idx;
    bool first = true;
    q_node_t *tail = &(tail_);
    q_node_t *cur_prev = prev_;
    q_node_t *first_new = nullptr;
    q_node_t *last_new = nullptr;

    while (to_process) {
      // Make new node
      auto new_target = q_node_t::make_node(rw, NUM_ELEMENTS);
      if (first) first_new = new_target;
      last_new = new_target;
      
      // Insert while the node is still captured
      int i;
      for (i = 0; i < NUM_ELEMENTS; i++) {
        if (i == to_process) break;
        new_target->elements[i].prio.set_cap(rw, new_target, batch[batch_idx].first);
        new_target->elements[i].job.set_cap(rw, new_target, batch[batch_idx++].second);
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
  std::optional<bool> enqueue_batch_vec(RWTX &rw, std::vector<std::pair<P,E>> &batch, int batch_size, bool &check) { // check == true (when passed) iff cur_num_lanes < MAX_QUEUES && current queue is last "active" one
    // Get tail & tail->prev
    q_node_t* tail = &tail_;
    auto target_o = tail->prev.get(rw, tail);
    if (!target_o)
      return {}; // ABORT!
    auto target = target_o.value();

    // If it's head, we need to insert a new node
    q_node_t* head = &(head_);
    if (target == head) {
      if (!ins_many(rw, batch, batch_size, 0, head))
        return {}; // ABORT!
      if (batch_size < NUM_ELEMENTS) 
        check = false;
      return true; // inserted to a new lane for first time - diffractor needs to increment num_queues
    }

    // Read the enqueue count
    auto enqs_o = target->enqueues.get(rw, target);
    if (!enqs_o)
      return {}; // ABORT!
    auto enqs = enqs_o.value();
    
    // TODO: should I fill the node tho?
    // Check if target is full
    int target_room = NUM_ELEMENTS - enqs;
    if (batch_size <= target_room) {
      // Room in target - fill it
      int batch_idx = 0;
      int e_idx;
      for (e_idx = enqs; e_idx < NUM_ELEMENTS; e_idx++) {
        if (batch_idx == batch_size) break;
        if (batch_idx == 0) {
          if (!(target->elements[e_idx].prio.set(rw, target, batch[batch_idx].first) &&
                target->elements[e_idx].job.set(rw, target, batch[batch_idx++].second))) return {}; // ABORT!
        } else {
          target->elements[e_idx].prio.set_mine(rw, target, batch[batch_idx].first);
          target->elements[e_idx].job.set_mine(rw, target, batch[batch_idx++].second);
        }
      }
      // Re-set target's enqs
      if (!target->enqueues.set(rw, target, e_idx)) return {}; // ABORT!
    } else {
      // Insert new nodes after target
      if (!ins_many(rw, batch, batch_size, 0, target)) return {}; // ABORT!

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

  /// Insert a batch into the queue
  ///
  /// Invariant: This never leaves an empty non-sentinel node in the queue
  ///
  /// @param rw       The current active transaction
  /// @param element  The elements to insert
  /// @param check    May initially be T or F - set to F if do not need to consider opening a new lane
  ///
  /// @return True if inserted to this queue for first time, False otherwise (std::nullopt for Abort)
  std::optional<bool> enqueue_batch_chunk(RWTX &rw, q_node_t* &batch, bool &check) { // check == true (when passed) iff cur_num_lanes < MAX_QUEUES && current queue is last "active" one
    // Get tail & tail->prev (target)
    q_node_t* tail = &tail_;
    auto target_o = tail->prev.get(rw, tail);
    if (!target_o)
      return {}; // ABORT!
    auto target = target_o.value();

    // 1. Lane is 100% empty - insert batch between head and tail
    if (target == &head_) {
      batch->prev.set_cap(rw, batch, &head_);
      batch->next.set_cap(rw, batch, tail);

      if (!(head_.next.set(rw, &head_, batch) &&
            tail->prev.set(rw, tail, batch))) {
        return {}; // ABORT!
      }
      if (check) check = false;

      //! `batch`: NULL'D
      // Set batch to nullptr - indicates NOT to re-use
      batch = nullptr;
      return true; // Inserted to lane for first time
    }
    
    // Get size of target
    auto target_size_o = target->enqueues.get(rw, target);
    if (!target_size_o) return {}; // ABORT!
    auto target_size = target_size_o.value();
    
    // 2. Target is empty -> swap in batch, and return target for re-use
    // TODO: Is it even possible that target is empty..? I think no
    if (target_size == 0) {
      auto target_prev_o = target->prev.get(rw, target);
      if (!target_prev_o) return {}; // ABORT!
      auto target_prev = target_prev_o.value();

      batch->prev.set_cap(rw, batch, target_prev);
      batch->next.set_cap(rw, batch, tail);

      if (!(target_prev->next.set(rw, target_prev, batch) && 
            tail->prev.set(rw, tail, batch))) {
        return {}; // ABORT!
      }
      if (check) check = false; // bc even if target->prev is head, still only 1 chunk inserted
      // Set batch to target for re-use

      //! `batch`: RE-USE `target` (store in `batch`)
      batch = target;
      if (!batch->enqueues.set(rw, batch, 0)) return {}; // ABORT!
      return false;
    }

    int batch_size = batch->enqueues.get_unsafe();

    // 3. Check if batch can fit completely within target:
    if ((batch_size + target_size) <= NUM_ELEMENTS) { //! need to make sure all chunks are num_elements length for this to work (or else, account for it otherwise - store max size in chunk when creating)
      // Copy over elements, then indicate to reuse batch
      int idx_batch = 0;
      int i;
      for (i = target_size; i < (target_size + batch_size); i++) {
        auto p_move = batch->elements[idx_batch].prio.get_unsafe();
        auto j_move = batch->elements[idx_batch++].job.get_unsafe();
        if (!(target->elements[i].prio.set(rw, target, p_move) &&
              target->elements[i].job.set(rw, target, j_move))) return {}; // ABORT!
      }
      if (!target->enqueues.set(rw, target, i)) return {}; // ABORT!
      // Re-use batch itself (i.e., do not set to nullptr)
      if (check) check = false; // bc didn't add a new chunk, so if prev is head, should be false
      //! `batch`: RE-USE
      if (!batch->enqueues.set(rw, batch, 0)) return {}; // ABORT!
      return false;
    } 

    // 4. Check if target is < 25% full
    if (target_size < (0.25 * NUM_ELEMENTS)) {
      // Copy over elements (starting at the end of `batch`, consuming in reverse from there)
      int idx_batch = batch_size - 1;
      int i;
      for (i = target_size; i < NUM_ELEMENTS; i++) {
        auto p_move = batch->elements[idx_batch].prio.get_unsafe();
        auto j_move = batch->elements[idx_batch--].job.get_unsafe();
        if (!(target->elements[i].prio.set(rw, target, p_move) &&
              target->elements[i].job.set(rw, target, j_move))) return {}; // ABORT!
      }
      // Reset enqueues of target and batch (recall we know batch could NOT fully fit within `target`)
      if (!target->enqueues.set(rw, target, NUM_ELEMENTS)) return {}; // ABORT!
      assert(idx_batch >= 0);
      if (!batch->enqueues.set(rw, batch, idx_batch + 1)) return {}; // ABORT!
    }

    // 5. Insert `batch` after `target`
    batch->prev.set_cap(rw, batch, target);
    batch->next.set_cap(rw, batch, tail);

    if (!(target->next.set(rw, target, batch) &&
          tail->prev.set(rw, tail, batch))) {
      return {}; // ABORT!
    }

    //! `batch`: NULL'D
    batch = nullptr;
    
    // Determine if new lane should be opened
    if (check) {
      // A new lane should be opened (i.e., keep check True) IF filling second chunk
      // Check if 'target' is first chunk
      auto target_prev_o = target->prev.get(rw, target);
      if (!target_prev_o)
        return {}; // ABORT!
      auto target_prev = target_prev_o.value();
      if (target_prev == &head_) 
        return false; // Leave `check` as true (new lane should be opened by caller)
      
      check = false;
    }
    return false;
  }

  /// ! [1/3/26] THIS IS OLD: It always inserts the whole chunk (only touches next/prev pointers), without regard for how full it is (i.e., a 128-element chunk may only contain 1-2 elements, if this repeats it wastes space)
  /// Insert an element into the queue
  ///
  /// Invariant: This never leaves an empty non-sentinel node in the queue
  ///
  /// @param rw       The current active transaction
  /// @param element  The elements to insert
  /// @param check    May initially be T or F - set to F if do not need to consider opening a new lane
  ///
  /// @return True if inserted to this queue for first time, False otherwise (std::nullopt for Abort)
  std::optional<bool> enqueue_batch(RWTX &rw, q_node_t* batch, bool &check) { // check == true (when passed) iff cur_num_lanes < MAX_QUEUES && current queue is last "active" one
    // Get tail & tail->prev
    q_node_t* tail = &tail_;
    auto target_o = tail->prev.get(rw, tail);
    if (!target_o)
      return {}; // ABORT!
    auto target = target_o.value();

    batch->prev.set_cap(rw, batch, target);
    batch->next.set_cap(rw, batch, tail);

    if (!(target->next.set(rw, target, batch) &&
          tail->prev.set(rw, tail, batch))) {
      return {}; // ABORT!
    }

    // Inserted first chunk to the queue
    if (target == &head_) {
      check = false;
      return true;
    }

    if (check) {
      // A new lane should be opened (i.e., keep check True) IF filling second chunk
      // Check if 'target' is first chunk
      auto target_prev_o = target->prev.get(rw, target);
      if (!target_prev_o)
        return {}; // ABORT!
      auto target_prev = target_prev_o.value();
      if (target_prev == &head_) 
        return false; // Leave `check` as true (new lane should be opened by caller)
      
      check = false;
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
};
