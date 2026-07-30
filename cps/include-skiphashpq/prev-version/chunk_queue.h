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

//!
//const int NUM_QUEUES = 2; // The total number of queues

/// A FIFO queue with coalescing to avoid excess allocations.  This queue
/// supports enqueue(), dequeue(), and empty() operations.
///
/// A key invariant is that there will never be an "empty chunk" in the queue
///
/// @tparam E       The type of elements stored in the queue
/// @tparam OPTSTM  A thread descriptor type
template <typename E, class OPTSTM> class chunk_queue {
  using ownable_t = typename OPTSTM::ownable_t;
  template <typename T> using FIELD = typename OPTSTM::template xField<T>;
  using ROTX = typename OPTSTM::RO;
  using RWTX = typename OPTSTM::RW;

public:

  struct q_node_t : ownable_t {
    FIELD<q_node_t *> prev;
    FIELD<q_node_t *> next;
    FIELD<uint32_t> enqueues;
    FIELD<E> elements[0];

    /// A queue node.  It has prev and next pointers, and an array of elements.
    /// We use ingress and egress counters to manage insertion/removal
    q_node_t()
        : ownable_t(), prev(nullptr), next(nullptr), enqueues(0) {}

    static q_node_t *make_node(RWTX &rw, uint32_t size) {
      uint32_t node_size = sizeof(q_node_t) + size * sizeof(FIELD<E>);
      void *region = malloc(node_size);
      return rw.LOG_NEW(new (region) q_node_t());
    }

    void dump(RWTX rw, int slot=-1) {
      if (slot >= 0) {
        std::cout << "[[ QUEUE-" << slot << " ]]: ";
      } else {
        std::cout << "[[ QUEUE ]]: [";
      }
      int size = enqueues.get(rw, this).value();
      for (int i = 0; i < size; i++) {
        std::cout << elements[i].get(rw, this).value() << ", ";
      }
      std::cout << "]\n";
    }
  };
  
  struct queue_t {
    q_node_t head; // The sentinel head (0 elements)
    q_node_t tail; // The sentinel tail (0 elements)

    queue_t() {}

    void init(RWTX &rw) {
      head.next.set_cap(rw, &head, &tail);
      tail.prev.set_cap(rw, &tail, &head);
    }
  };

  const int NUM_ELEMENTS; // The size of each vector

  // counter_t nqs;
  queue_t q_;

  /// Default construct a queue from within a RW transactional context
  ///
  /// @param rw         The currently active RW transaction
  /// @param chunksize  The size of each chunk in the queue
  chunk_queue(RWTX &rw, uint32_t chunksize) : NUM_ELEMENTS(chunksize), q_() {
    q_.init(rw);
  }

  /// Insert an element into the queue
  ///
  /// Invariant: This never leaves an empty non-sentinel node in the queue
  ///
  /// @param rw       The current active transaction
  /// @param element  The element to insert'
  /// @param 
  ///
  /// @return true if inserted to this queue for first time, false otherwise (std::nullopt for Abort)
  std::optional<bool> enqueue(RWTX &rw, E &element, bool &check) {
    // Get tail
    q_node_t* tail = &(q_.tail);

    // Get node before tail
    auto target_o = tail->prev.get(rw, tail);
    if (!target_o)
      return {}; // ABORT!
    auto target = target_o.value();

    // If it's head, we need to insert a new node
    q_node_t* head = &(q_.head);
    if (target == head) {
      // Make new node
      auto new_target = q_node_t::make_node(rw, NUM_ELEMENTS);
      // Insert while the node is still captured
      new_target->enqueues.set_cap(rw, target, 1);
      new_target->elements[0].set_cap(rw, target, element);
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
      new_target->enqueues.set_cap(rw, target, 1);
      new_target->elements[0].set_cap(rw, target, element);
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
          target->elements[enqs].set(rw, target, element))) {
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

  /// Remove a chunk from the queue
  ///
  /// Invariant: This never leaves an empty non-sentinel node in the queue
  ///
  /// @param rw The current active transaction
  /// @param empty_q Initially false, to be set true if queue becomes empty
  ///
  /// @return A chunk from the queue, or NONE on abort
  std::optional<std::pair<q_node_t*, RWTX>> dequeue(OPTSTM *me, ROTX &ro, bool &empty_q) {
    // Get target to unstitch from
    q_node_t* head = &(q_.head);
    auto target_o = head->next.get(ro, head);
    if (!target_o) return {}; // ABORT!
    q_node_t* target = target_o.value();

    //! End read-only transaction
    me->end_ro();
    //! Start read-write transaction
    RWTX rw(me);

    // Re-read target
    target_o = head->next.get(rw, head);
    if (!target_o) return {}; // ABORT!
    target = target_o.value();

  #ifndef NDEBUG
    assert(!empty_q);
    assert(!(target == &(q_.tail)));
  #endif

    // Get target's successor
    auto next_o = target->next.get(rw, target);
    if (!next_o)
      return {}; // ABORT!
    auto next = next_o.value();
    // Unstitch target
    if (!head->next.set(rw, head, next)) return {}; // ABORT!
    if (!next->prev.set(rw, next, head)) return {}; // ABORT!

    // Check if now empty
    if (next == &(q_.tail)) {
      empty_q = true;
    }
    return std::make_pair(target, rw);
  }

  // SINGLE threaded only
  void print_queues(RWTX &rw) {
    q_node_t* head = &(q_.head);
    q_node_t* tail = &(q_.tail);

    q_node_t* cur = head->next.get(rw, head).value();
    bool first = true;
    while (cur != tail) {
      if (first) {
        std::cout << "[ ";
        first = false;
      } else {
        std::cout << "-> [ ";
      }

      int size = cur->enqueues.get(rw, cur).value();
      for (int j = 0; j < size; j++) {
        std::cout << cur->elements[j].get(rw, cur).value() << " ";
      }
      std::cout << "] ";

      cur = cur->next.get(rw, cur).value();
    }
  }

  /// Report if the queue is empty
  ///
  /// @param rw The current active transaction
  ///
  /// @return True if it's empty, false if not, NONE on abort
  std::optional<bool> empty(RWTX &rw) {
    auto next_o = q_.head.next.get(rw, &(q_.head));
    if (!next_o) return {}; // ABORT!
    return (next_o.value() == &(q_.tail));
  }

  template <typename TX> std::pair<int,long> dump(TX &ro, bool print=false) {
    int num_elems = 0;
    long key_sum = 0;
    auto curr = q_.head.next.get(ro, &(q_.head)).value();

    if (print) std::cout << "{\n";
    
    while (curr != &(q_.tail)) {
      int size = curr->enqueues.get(ro, curr).value();
      num_elems += size;
      
      for (int j = 0; j < size; j++) {
        auto val = curr->elements[j].get(ro, curr).value();
        if (print) std::cout << val << ", ";
        key_sum += val;
      }
      if (print) std::cout << " ], ";
      curr = curr->next.get(ro, curr).value();
    }
    if (print) std::cout << "}\n";
    return std::make_pair(num_elems, key_sum);
  }
};
