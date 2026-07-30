// 2024-11-21: This data structure is functional.  There are a few outstanding
// issues:
//
// -  Consider switching to a skip list that is unrolled in the data layer?
// -  Consider an adaptive height mechanism in the skip list, so that towers
//    don't get unnecessarily big?
// -  Consider adding a cache of the max tower height to the skip list, to avoid
//    the downward checking step?
// -  Consider switching to a resizable caucmap, instead of the adapter, so that
//    we can get better worst-case guarantees?
// -  Relax the data structure in one or more ways, to reduce the frequency of
//    operations on shared memory?
// -  Be sure to test using jemalloc, since we do have some memory churn!
// -  Consider caching discarded dclist nodes, instead of sending them to SMR?
// -  Consider caching discarded skip list nodes, instead of sending to SMR?

#pragma once

#include "caucmap_adapter.h"
#include "chunk_queue.h"
#include "chunk_queue_diffractor.h"
#include "dclist_omap.h"
#include "skipslist_oset.h"

#define SKIPHASH_PQ_RELAXED

/// A priority queue, implemented as a skip list of priorities and a hash table
/// that maps priorities to queues of tasks.
///
/// @tparam P       The type of priorities stored in this priority queue
/// @tparam J       The type of jobs stored in this priority queue
/// @tparam OPTSTM  A thread descriptor type, for safe memory reclamation
template <typename P, typename J, class OPTSTM> class skiphash_pq_relaxed {
  using RO = typename OPTSTM::RO;
  using RW = typename OPTSTM::RW;

  using SL_T = skipslist_oset<P, OPTSTM>;
  using Q_T = chunk_queue<J, OPTSTM>;
  using QD_T = chunk_queue_diffractor<Q_T, J, OPTSTM>;
  using DCL_T = dclist_omap<P, QD_T, OPTSTM>;
  using UMAP_T = caucmap_adapter<P, QD_T, OPTSTM, DCL_T>;

  /// Thread local pointer to thread-owned "local" node to be processed by del-mins
  inline static thread_local typename Q_T::q_node_t* t_remove_arr;
  inline static thread_local int t_cur_idx;
  inline static thread_local int t_cur_p;

  /// Used post-experiment to store thread-local data for keysum and size validations
  /// Only used once experiment has ended (end time has been read)
  std::mutex global_mutex;
  std::vector<typename Q_T::q_node_t *> collected_remove_arr;
  std::vector<int> collected_num_items;

  /// Global data structures
  UMAP_T jobs;     // The unordered map of queues of jobs
  SL_T priorities; // The skip list of priorities

public:
  template <typename config_t>
  skiphash_pq_relaxed(OPTSTM *me, config_t *cfg) : jobs(me, cfg), priorities(me, cfg) {}

  /// Insert a job into the priority queue
  ///
  /// @param me   The caller's thread context
  /// @param prio The priority for the new job
  /// @param job  The new job
  void insert(OPTSTM *me, P prio, J job) {
    // Since we're using OPTSTM2, we need to manually handle rollback:
    while (true) {
      RW rw(me);
      
      // Get the collection where this job should go.  If it returns nullptr,
      // the priority doesn't exist yet in the PQ, so we'll need to make a
      // collection and ultimately update the skip list.
      //
      // [mfs] This faux upsert is kind of ugly.  Redesign the API?
      auto c_o = jobs.get(rw, prio);
      if (!c_o)
        continue;
      auto c = c_o.value();
      // If this is a new priority, then make a collection
      bool newprio = (c == nullptr);
      if (c == nullptr) {
        auto o = jobs.make_collection(rw, prio);
        if (!o)
          continue;
        c = o.value();
      }
      // Put the job into the collection
      if (!c->enqueue(rw, job))
        continue;
      // Update the skip list, if necessary
      if (newprio && !priorities.insert_guaranteed(rw, prio))
        continue;
      if (!me->try_end_rw())
        continue;
      return;
    }
  }

  /// Get the highest priority job out of the priority queue
  ///
  /// [mfs] Are the semantics right here?  What if the queue is empty?
  ///
  /// @param me The caller's thread context
  ///
  /// @return The job that was found
  std::pair<P, J> extract_min(OPTSTM *me) {
    // first consult thread-local array
    if (t_remove_arr != nullptr) {
      // get next job to return, then increment local index
      auto ret_job = t_remove_arr->elements[t_cur_idx++].get_unsafe();

      // check if need to reclaim
      check_reclaim();

      return std::make_pair(t_cur_p, ret_job);
    }

    // consult global structure
    while (true) {
      RO ro(me);
      // RW rw(me);

      // Check if skiplist is empty
      auto e_o = priorities.empty(ro);
      if (!e_o)
        continue;
      if (e_o.value()) {
        me->end_ro();
        return std::make_pair(INT_MAX, INT_MAX); // PQ is empty
      }

      // Get the highest priority
      auto p_o = priorities.front(ro);
      if (!p_o)
        continue;
      auto p = p_o.value();

      // Get the queue (collection) for that priority
      auto q_o = jobs.get(ro, p);
      if (!q_o)
        continue;
      auto q = q_o.value();

      // END READ-ONLY TRANSACTION
      //me->end_ro();
      //! DECLARE READ-WRITE TRANSACTION (but do not initialize yet)
      // RW* rw;

      bool empty_q = false; // reset to true in dequeue() if all queues become empty
      auto chunk_o = q->dequeue(me, ro, empty_q); //! NOTE: if this returns without abort, then ro has been closed, and rw opened
      if (!chunk_o)
        continue;
      RW rw = chunk_o.value().second;

      // store locally
      t_remove_arr = chunk_o.value().first;
      t_cur_p = p;
      // Pull a job out
      auto j_o = t_remove_arr->elements[0].get(rw, t_remove_arr);
      if (!j_o) {
        t_remove_arr = nullptr;
        continue;
      }
      t_cur_idx = 1;

      // If we made the selected queue empty, we may have more to do (if all are empty)
      if (empty_q) {
        // All queues empty -> remove the queue from the hash table of queues
        auto r_o = jobs.remove(rw, p);
        if (!r_o) {
          t_remove_arr = nullptr;
          continue;
        }
          
        // Remove the priority from the skip list
        auto rr_o = priorities.remove_front(rw);
        if (!rr_o) {
          t_remove_arr = nullptr;
          continue;
        }
      }
      
      if (!me->try_end_rw()) {
        t_remove_arr = nullptr;
        continue;
      }

      // check if need to reclaim
      check_reclaim();

      return std::make_pair(t_cur_p, j_o.value());
    }
  }

  /// Check if t_remove_arr needs to be reclaimed (i.e., we have exhausted all items in the node with extract_min() calls)
  /// NOTE: do not call within a transaction, since 'delete t_remove_arr;' cannot be undone
  ///
  /// @param rw A read/write transactional context
  /// 
  /// @return true if no abort (i.e., true doesn't indicate if t_remove_arr was reclaimed or not -
  ///           we do not need to know whether it was reclaimed or not since if it is, t_remove_arr is set to nullptr)
  void check_reclaim() {
   #ifndef NDEBUG
    assert(t_remove_arr != nullptr);
   #endif

    auto num_items = t_remove_arr->enqueues.get_unsafe();
    
   #ifndef NDEBUG
    assert(!(t_cur_idx > num_items));
   #endif

    if (t_cur_idx == num_items) {
      delete t_remove_arr;
      t_remove_arr = nullptr;
    }
  }

  /// Report if the priority queue is empty
  ///
  /// @param me The caller's thread context
  ///
  /// @return True if the pq is empty, false otherwise
  bool empty(OPTSTM *me) {
    while (true) {
      RO ro(me);
      auto res = priorities.empty(ro);
      if (!res)
        continue;
      ro.OP()->end_ro();
      return res.value();
    }
  }

  /// Debug: Print the data structure
  std::pair<int,long> dump(OPTSTM *me) {
    RO ro(me);
    //priorities.dump(ro);
    std::pair<int,long> ret = jobs.dump(ro);

    // parse collected local arrays
    for (int i = 0; i < collected_remove_arr.size(); i++) {
      auto max = collected_remove_arr[i]->enqueues.get(ro, collected_remove_arr[i]).value();

      for (int j = collected_num_items[i]; j < max; j++) {
        auto key = collected_remove_arr[i]->elements[j].get(ro, collected_remove_arr[i]).value();
        ret.first++;
        ret.second += key;
      }
    }

    me->end_ro();
    return ret;
  }

  // Accumulates locally stored elements into global vectors
  // To be called by each thread before termination for keysum check
  void thread_terminate() {
    // add t_remove_arr to vector
    std::lock_guard<std::mutex> lock(global_mutex);
    if (t_remove_arr != nullptr) {
      collected_remove_arr.push_back(t_remove_arr);
      collected_num_items.push_back(t_cur_idx);
    }
  }
};
