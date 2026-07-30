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

#include "random_num.h"

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
    // FIELD<uint32_t> dequeues;
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
  
  struct queue_t : ownable_t {
    q_node_t head; // The sentinel head (0 elements)
    q_node_t tail; // The sentinel tail (0 elements)

    queue_t() : ownable_t() {}

    void init(RWTX &rw) {
      head.next.set_cap(rw, &head, &tail);
      tail.prev.set_cap(rw, &tail, &head);
    }
  };

  struct q_ptr_t : ownable_t {
    FIELD<queue_t*> queue;
    
    q_ptr_t() : ownable_t() {}
  };

  struct counter_t : ownable_t {
    FIELD<uint8_t> num_queues; // the number of non-empty queues (0-MAX_QUEUES)
    FIELD<uint8_t> open_lanes; // number of lanes (queues) that an insert may insert to (1-MAX_QUEUES)
    counter_t() : ownable_t(), num_queues(0), open_lanes(1) {}
  };

  inline static thread_local random_num rand_engine;

  const int NUM_ELEMENTS; // The size of each vector
  const int MAX_QUEUES;
  
  counter_t nqs;
  q_ptr_t* q_wrapper;

  /// Default construct a queue from within a RW transactional context
  ///
  /// @param rw         The currently active RW transaction
  /// @param chunksize  The size of each chunk in the queue
  chunk_queue(RWTX &rw, uint32_t chunksize, uint32_t n_queues) :
        NUM_ELEMENTS(chunksize),
        MAX_QUEUES(n_queues),
        nqs() {
    q_wrapper = rw.LOG_NEW(new q_ptr_t[MAX_QUEUES]);
    for (int i = 0; i < MAX_QUEUES; i++) {
      auto ptr = rw.LOG_NEW(new queue_t());
      if (!q_wrapper[i].queue.set(rw, &q_wrapper[i], ptr)) exit(0);
      // call init()
      auto q_ptr = q_wrapper[i].queue.get(rw, &q_wrapper[i]).value();
      q_ptr->init(rw);
    }
  }

  uint64_t random_number(int max_k) {
    std::uniform_int_distribution<int> rng_(0, max_k);
    return rng_(rand_engine);
  }

  /// Insert an element into the queue
  ///
  /// Invariant: This never leaves an empty non-sentinel node in the queue
  ///
  /// @param rw       The current active transaction
  /// @param element  The element to insert
  ///
  /// @return True if the element was inserted, false on abort
  //! increment counter once we happen to choose the last one, and there is exactly one chunk which becomes filled
  bool enqueue(RWTX &rw, E &element) {
    // Generate a random bucket to insert to
    uint64_t rand_queue_idx = 0;
    auto cur_num_lanes_o = nqs.open_lanes.get(rw, &nqs);
    if (!cur_num_lanes_o) return {}; // ABORT!
    auto cur_num_lanes = cur_num_lanes_o.value();
    if (cur_num_lanes > 1) {
      rand_queue_idx = random_number(cur_num_lanes - 1);
    }

    // Get tail
    auto q_ptr_o = q_wrapper[rand_queue_idx].queue.get(rw, &(q_wrapper[rand_queue_idx]));
    if (!q_ptr_o) return {}; // ABORT!
    auto q_ptr = q_ptr_o.value();
    q_node_t* tail = &(q_ptr->tail);

    // Get node before tail
    auto target_o = tail->prev.get(rw, tail);
    if (!target_o)
      return false; // ABORT!
    auto target = target_o.value();

    // If it's head, we need to insert a new node
    q_node_t* head = &(q_ptr->head);
    if (target == head) {
      // Make new node
      auto new_target = q_node_t::make_node(rw, NUM_ELEMENTS);
      // Insert while the node is still captured
      new_target->enqueues.set_cap(rw, target, 1);
      new_target->elements[0].set_cap(rw, target, element);
      // Stitch between head and tail
      new_target->prev.set_cap(rw, new_target, head);
      new_target->next.set_cap(rw, new_target, tail);

      // Get and then increment number of queues
      auto cur_num_queues_o = nqs.num_queues.get(rw, &nqs);
      if (!cur_num_queues_o) return {}; // ABORT!
      auto cur_num_queues = cur_num_queues_o.value();
      if (!nqs.num_queues.set(rw, &nqs, cur_num_queues + 1)) return {}; // ABORT!

      // The last stitching will either abort us or finish the method
      return head->next.set(rw, head, new_target) &&
             tail->prev.set(rw, tail, new_target);
    }

    // Read the enqueue count
    auto enqs_o = target->enqueues.get(rw, target);
    if (!enqs_o)
      return false; // ABORT!
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
      return target->next.set(rw, target, new_target) &&
             tail->prev.set(rw, tail, new_target);
    }

    // TODO: change this logic (to properly use nqs.num_queues and nqs.open_lanes)
    // TODO: ^ can the check if we should initiate a new lane be wrt to open_lanes vs num_queues, or does the following logic hold? I think the latter bc the point is that if the last is being filled for the first time, we know it was the most recent one opened, so we should initiate the new lane
    // Check if we should initiate a new lane
    if (cur_num_lanes < MAX_QUEUES &&              // haven't reached max lanes
        rand_queue_idx == (cur_num_lanes - 1) &&   // at the current last queue
        enqs == (NUM_ELEMENTS - 1)) {               // chunk will become full with insertion 
      
      // check if only one chunk currently
      auto target_prev_o = target->prev.get(rw, target);
      if (!target_prev_o)
        return false; // ABORT!
      auto target_prev = target_prev_o.value();
      if (target_prev == head) {
        if (!nqs.open_lanes.set(rw, &nqs, cur_num_lanes + 1)) return false; // ABORT!
      }
    }

    // Update the enqueue count, put the element in
    return target->enqueues.set(rw, target, enqs + 1) &&
           target->elements[enqs].set(rw, target, element);
  }

  /// Remove a chunk from the queue
  ///
  /// Invariant: This never leaves an empty non-sentinel node in the queue
  ///
  /// @param rw The current active transaction
  ///
  /// @return A chunk from the queue, or NONE on abort
  std::optional<q_node_t*> dequeue(RWTX &rw, bool &empty_q) {
    // Generate a random queue to remove from
    uint64_t rand_queue_idx = 0;
    auto cur_num_queues_o = nqs.num_queues.get(rw, &nqs);
    if (!cur_num_queues_o || !cur_num_queues_o.value()) return {}; // ABORT!
    uint8_t cur_num_queues = cur_num_queues_o.value();
    if (cur_num_queues > 1) {
      rand_queue_idx = random_number(cur_num_queues - 1);
    } 

    // Read head of desired queue
    auto q_ptr_o = q_wrapper[rand_queue_idx].queue.get(rw, &(q_wrapper[rand_queue_idx]));
    if (!q_ptr_o) return {}; // ABORT!
    auto q_ptr = q_ptr_o.value();
    q_node_t* head = &(q_ptr->head);

    // Get target to unstitch
    auto target_o = head->next.get(rw, head);
    if (!target_o) return {}; // ABORT!
    q_node_t* target = target_o.value();

    //! issue: if we "open" a lane but do not insert to it, then num_queue's is "invalid" wrt dequeue - either change the opening lane logic, or handle this case in dequeue - but then how to know when empty ??
    //! ^ so prob want to change opening a lane logic

  #ifndef NDEBUG
    // if (target == &(q_ptr->tail)) {
    //   std::cout << "cur_num_queues: " << (int)cur_num_queues << ", rand_queue_idx: " << rand_queue_idx << "\n";
    // }
    assert(!(target == &(q_ptr->tail)));
  #endif

    // Get target's successor
    auto next_o = target->next.get(rw, target);
    if (!next_o)
      return {}; // ABORT!
    auto next = next_o.value();
    // Unstitch target and reclaim it
    if (!head->next.set(rw, head, next)) return {}; // ABORT!
    if (!next->prev.set(rw, next, head)) return {}; // ABORT!

    // Check if now empty
    if (next == &(q_ptr->tail)) {
      if (rand_queue_idx < (cur_num_queues - 1)) {
        // Swap queue at last slot to here
        if (!swap(rw, rand_queue_idx, cur_num_queues)) return {}; // ABORT!
      }
      // Read open_lanes
      auto cur_open_lanes_o = nqs.open_lanes.get(rw, &nqs);
      if (!cur_open_lanes_o) return {}; // ABORT!
      auto cur_open_lanes = cur_open_lanes_o.value();

      if (cur_open_lanes > 1) {
        // Decrement open_lanes
        if (!nqs.open_lanes.set(rw, &nqs, cur_open_lanes - 1)) return {}; // ABORT!
      }

      // decrement num_queues
      if (!nqs.num_queues.set(rw, &nqs, cur_num_queues - 1)) return {}; // ABORT!

      // check if empty
      empty_q = ((cur_num_queues - 1) == 0);
    }
    return target;
  }

  bool swap(RWTX &rw, int swap_idx, int n_queues) {
    auto last_q_ptr_o = q_wrapper[n_queues - 1].queue.get(rw, &(q_wrapper[n_queues - 1]));
    if (!last_q_ptr_o) return false; // ABORT!
    auto last_q_ptr = last_q_ptr_o.value();

    auto swap_q_ptr_o = q_wrapper[swap_idx].queue.get(rw, &(q_wrapper[swap_idx]));
    if (!swap_q_ptr_o) return false; // ABORT!
    auto swap_q_ptr = swap_q_ptr_o.value();

    return q_wrapper[n_queues - 1].queue.set(rw, &q_wrapper[n_queues - 1], swap_q_ptr) &&
           q_wrapper[swap_idx].queue.set(rw, &q_wrapper[swap_idx], last_q_ptr);
  }

  // SINGLE threaded only
  void print_queues(RWTX &rw) {
    auto cur_num_queues = nqs.num_queues.get(rw, &nqs).value();

    for (int i = 0; i < cur_num_queues; i++) {
      q_node_t* head = &(q_wrapper[i].queue.head.get().value());
      q_node_t* tail = &(q_wrapper[i].queue.tail.get().value());

      q_node_t* cur = head->next.get(rw, head).value();
      std::cout << "[queue_num=" << i << "]: ";
      while (cur != tail) {
        std::cout << "-> [ ";
        int size = cur->enqueues.get(rw, cur).value();
        for (int j = 0; j < size; j++) {
          std::cout << cur->elements[j].get(rw, cur).value() << " ";
        }
        std::cout << "] ";

        cur = cur->next.get(rw, cur).value();
      }
      std::cout << "\n";
    }
  }

  /// Report if the queue is empty
  ///
  /// @param rw The current active transaction
  ///
  /// @return True if it's empty, false if not, NONE on abort
  std::optional<bool> empty(RWTX &rw) {
    auto num_queues_o = nqs.num_queues.get(rw, &nqs);
    if (!num_queues_o) return {}; // ABORT!
    return (num_queues_o.value() == 0);
  }

  template <typename TX> std::pair<int,long> dump(TX &ro, bool print=false) {
    int num_elems = 0;
    long key_sum = 0;
    if (print) std::cout << "{\n";
    for (int i = 0; i < MAX_QUEUES; i++) {
      auto q_ptr = q_wrapper[i].queue.get(ro, &(q_wrapper[i])).value();
      auto curr = q_ptr->head.next.get(ro, &(q_ptr->head)).value();

      if (print) std::cout << "[[ QUEUE-" << i << " ]]: ";
      
      while (curr != &(q_ptr->tail)) {
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
      if (print) std::cout << "\n";
    }
    if (print) std::cout << "}\n";
    return std::make_pair(num_elems, key_sum);
  }
};
