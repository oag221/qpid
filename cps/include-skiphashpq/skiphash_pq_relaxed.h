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

#include "chunk_pool.h"

#include <sstream>
#include <iostream>

// #include "spdlog/spdlog.h"
// #include "spdlog/async.h"                // Fixes init_thread_pool and async_factory
// #include "spdlog/sinks/basic_file_sink.h" // Fixes basic_logger_mt

// Usage: DEBUG_MSG("Index: " << idx << " Thread: " << thread_id);
#ifndef DEBUG_MSG
#define DEBUG_MSG(msg) \
  { \
      std::stringstream ss; \
      ss << "[Thread " << std::this_thread::get_id() << "] " << msg << "\n"; \
      std::cout << ss.str(); \
  }
#endif

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
  using Q_T = chunk_queue<P, J, OPTSTM>;
  using QD_T = chunk_queue_diffractor<Q_T, P, J, OPTSTM>;
  using DCL_T = dclist_omap<P, QD_T, OPTSTM>;
  using UMAP_T = caucmap_adapter<P, QD_T, OPTSTM, DCL_T>;

  using chunk_t = typename Q_T::q_node_t;
  using kv_t = typename chunk_t::kv_t;

  #ifdef CHUNK_POOL
  using pool_t = chunk_pool<P, J, OPTSTM>;
  std::vector<pool_t> thread_pools;
  static inline thread_local pool_t* t_local_pool = nullptr;
  #endif

  /// Thread local pointer to thread-owned "local" chunk to be processed by del-mins
  inline static thread_local chunk_t* t_remove_arr = nullptr;
  inline static thread_local int t_cur_idx = 0;
  
  inline static thread_local std::vector<kv_t> t_ins_vec;
  inline static thread_local int t_ins_vec_idx;

 #ifdef PROFILING
  /// Used post-experiment to store thread-local data for keysum and size validations
  /// Only used once experiment has ended (end time has been read)
  std::mutex global_mutex;
  std::vector<chunk_t *> collected_remove_arr;
  std::vector<int> collected_num_items;
 
  int collected_num_allocs;
  int collected_num_pool_use;
  inline static thread_local int t_num_allocs = 0;
  inline static thread_local int t_num_pool_use = 0;
 #endif

  /// Global data structures
  UMAP_T jobs;     // The unordered map of queues of jobs
  SL_T priorities; // The skip list of priorities

  const int MAX_BATCH_SIZE;
  const int CHUNK_SIZE;
  const int DELTA;

public:
  template <typename config_t>
  skiphash_pq_relaxed(OPTSTM *me, config_t *cfg) : jobs(me, cfg), priorities(me, cfg), MAX_BATCH_SIZE(cfg->max_batch_size), CHUNK_SIZE(cfg->chunksize), DELTA(cfg->delta) {
    #ifdef CHUNK_POOL
    for (int i = 0; i < cfg->threads; i++) {
      thread_pools.push_back(pool_t(CHUNK_SIZE, cfg->pool_reserve, cfg->pool_init_chunks, me));
    }
    #endif

    #ifdef PROFILING
    std::cout << "WARNING: PROFILING is defined - should not be defined if running tests.\n";
    #endif
  }

  void init_thread(OPTSTM *me, int tid) {
    t_ins_vec.resize(CHUNK_SIZE + 1);
    t_ins_vec_idx = 0;
    #ifdef CHUNK_POOL
    if (!t_local_pool) t_local_pool = &thread_pools[tid];
    #endif
  }

  void re_init_thread(OPTSTM *me) {
    #ifdef CHUNK_POOL
    t_local_pool->pool_init(me);
    #elif defined(PROFILING)
    t_num_allocs = 0;
    t_num_pool_use = 0;
    #endif
  }

  /// Insert a job into the priority queue
  ///
  /// @param me   The caller's thread context
  /// @param prio The priority for the new job
  /// @param job  The new job
  /// ! API !
  void insert(OPTSTM *me, P prio, J job) {
    auto delta_p = prio >> DELTA;
    // Since we're using OPTSTM2, we need to manually handle rollback:
    while (true) {
      RW rw(me);
      
      // Get the collection where this job should go.  If it returns nullptr,
      // the priority doesn't exist yet in the PQ, so we'll need to make a
      // collection and ultimately update the skip list.
      //
      // [mfs] This faux upsert is kind of ugly.  Redesign the API?
      auto c_o = jobs.get_ins(rw, delta_p);
      if (!c_o)
        continue;
      auto c = c_o.value();
      // If this is a new priority, then make a collection
      bool newprio = (c == nullptr);
      if (c == nullptr) {
        auto o = jobs.make_collection(rw, delta_p);
        if (!o)
          continue;
        c = o.value();
      }
      // Put the job into the collection
     #ifdef CHUNK_POOL
      bool used_pool;
      if (!c->enqueue(rw, *t_local_pool, std::ref(used_pool), prio, job))
        continue;
     #else
      if (!c->enqueue(rw, prio, job))
        continue;
     #endif
      // Update the skip list, if necessary
      if (newprio && !priorities.insert_guaranteed(rw, delta_p))
        continue;
      if (!me->try_end_rw())
        continue;
     #ifdef CHUNK_POOL
      if (used_pool) {
        // only remove the chunk once the operation succeeds
        t_local_pool->pool_remove_chunk();
      }
     #ifdef PROFILING
      auto ret = t_local_pool->check_prof_fields();
      if (ret) {
        if (ret.value()) t_num_pool_use++;
        else t_num_allocs++;
      }
     #endif
     #endif
      return;
    }
  }

  /// Batch Insert API
  /// Invariant: there is room to insert into the batch
  ///
  /// @param ordered Whether (true) or not (false) to order the batch before inserting its elements
  void insert_batch(OPTSTM *me, P prio, J job, bool ordered=false) {
    // Insert to batch
    t_ins_vec[t_ins_vec_idx].prio.set_unsafe(prio);
    t_ins_vec[t_ins_vec_idx++].job.set_unsafe(job);
    // Perform flush if necessary
    flush_batch(me, false, ordered);
  }

  /// Flush the batch (if conditions hold)
  ///
  /// @param force Whether (true) or not (false) to flush even if not full (as long as also non-empty)
  /// @param ordered Whether (true) or not (false) to order the batch before flushing it
  void flush_batch(OPTSTM *me, bool force=false, bool ordered=false) {
    // Batch is full, or is non-empty and being forced to be flushed
    if ((t_ins_vec_idx == CHUNK_SIZE) || (force && t_ins_vec_idx)) {
      if (ordered) {
        // First, sort the batch
        std::sort(t_ins_vec.begin(), t_ins_vec.begin() + t_ins_vec_idx, [](kv_t& a, kv_t& b) {
          return a.prio.get_unsafe() < b.prio.get_unsafe();
        });
      }

      // Flush to global
      int cur_idx = 0;
      while (cur_idx < t_ins_vec_idx) {
        int start_idx = cur_idx;
        auto cur_delt_p = t_ins_vec[cur_idx].prio.get_unsafe() >> DELTA;
        auto prev_delta_p = cur_delt_p;
        // Insert contiguous elements of the same priority together
        while ((cur_idx < t_ins_vec_idx) && (prev_delta_p == cur_delt_p)) {
          cur_delt_p = t_ins_vec[++cur_idx].prio.get_unsafe() >> DELTA;
        }
        // Insert vec[start_idx:cur_idx]
        int batch_size = cur_idx - start_idx;
        if (batch_size == 1) {
          insert(me, prev_delta_p, t_ins_vec[start_idx].job.get_unsafe());
        } else {
          std::vector<kv_t> vec_sub(t_ins_vec.begin() + start_idx, t_ins_vec.begin() + cur_idx);
          insert_batch_internal(me, prev_delta_p, vec_sub, batch_size);
        }
      }
      // Reset index into vector
      t_ins_vec_idx = 0;
    }
  }

  /// (Batch Implementation) Remove a highest priority element from the PQ 
  ///
  /// Invariant: if t_remove_arr is not nullptr, there is at least one more element to be consumed
  ///
  /// @param me The caller's thread context
  ///
  /// @return The job that was found
  std::optional<std::pair<P,J>> extract_min(OPTSTM *me) {
    // first consult thread-local array
    if (t_remove_arr != nullptr) {
      auto ret_p = t_remove_arr->elements[t_cur_idx].prio.get_unsafe();
      auto ret_j = t_remove_arr->elements[t_cur_idx++].job.get_unsafe();

      // check if need to reclaim
      check_reclaim(me);

      return std::make_pair(ret_p, ret_j);
    }

    // consult global structure
    while (true) {
      RO ro(me);

      // Check if skiplist is empty
      auto e_o = priorities.empty(ro);
      if (!e_o)
        continue;
      if (e_o.value()) {
        me->end_ro();
        return {}; // PQ is empty
      }

      // Get the highest priority
      auto p_o = priorities.front(ro);
      if (!p_o)
        continue;
      auto p = p_o.value();

      //! END RO
      me->end_ro();
      //! START RW
      RW rw(me);

      // Get the queue (collection) for that priority
      auto ret_o = jobs.get_extract(rw, p);
      if (!ret_o)
        continue;
      auto q = ret_o.value();

      if (q == nullptr) { // key not found, try again
        // force abort and re-start
        me->force_abort(rw);
        continue;
      }

      bool empty_q = false; // reset to true in dequeue() if all queues become empty
      auto chunk_o = q->dequeue(rw, empty_q);
      if (!chunk_o)
        continue;

      // Store locally
      t_remove_arr = chunk_o.value();
      
      // Set next pointer of t_remove_arr to nullptr, just to gain ownership of the orec
      // Without a successful set() on the orec, it is possible subsequent local (`unsafe`) operations see temporarily incorrect values
      if (!t_remove_arr->next.set(rw, t_remove_arr, nullptr)) {
        t_remove_arr = nullptr;
        continue;
      }

      // Pull a job out
      auto ret_p = t_remove_arr->elements[0].prio.get_unsafe();
      auto ret_j = t_remove_arr->elements[0].job.get_unsafe();
      t_cur_idx = 1;

      // Check if collection is empty
      if (empty_q) {
        // Re-read the highest priority
        auto p2_o = priorities.front(rw);
        if (!p2_o) {
          t_remove_arr = nullptr;
          continue;
        }
          
        auto p2 = p2_o.value();
        // Force-Abort (restart) if priority has changed
        if (p != p2) {
          t_remove_arr = nullptr;
          me->force_abort(rw);
          continue;
        }

        // All queues empty -> remove the queue from the hash table of queues
        auto r_o = jobs.remove(rw, p);
        if (!r_o) {
          t_remove_arr = nullptr;
          continue;
        }
          
        // Remove the priority from the skip list
        auto rr_o = priorities.remove_front(rw, p);
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
      check_reclaim(me);

      return std::make_pair(ret_p, ret_j);
    }
  }

  /// Check if t_remove_arr needs to be reclaimed (i.e., we have exhausted all items in the node with extract_min() calls)
  ///
  /// @param rw A read/write transactional context
  void check_reclaim(OPTSTM *me) {
    auto num_items = t_remove_arr->enqueues.get_unsafe();
    if (t_cur_idx == num_items) {
     #ifdef CHUNK_POOL
      t_remove_arr->reset();
      t_local_pool->pool_insert_chunk(t_remove_arr);
     #else
      while (true) {
        RW rw(me);
        rw.reclaim(t_remove_arr);
        if (me->try_end_rw())
          break;
      }
     #endif
      t_remove_arr = nullptr;
    }
  }

  /// (STRICT implementation) Get the highest priority job out of the priority queue
  ///
  /// @param me The caller's thread context
  ///
  /// @return The prio-job pair that was found
  std::optional<std::pair<P,J>> extract_min_strict(OPTSTM *me) {
    while (true) {
     #ifdef CHUNK_POOL
      typename Q_T::q_node_t* retired_chunk = nullptr;
     #endif

      RO ro(me);

      // Check if skiplist is empty
      // todo: instead of checking if empty, have .front() below return nullptr if empty
      auto e_o = priorities.empty(ro);
      if (!e_o)
        continue;
      if (e_o.value()) {
        me->end_ro();
        return {}; // PQ is empty
      }

      // Get the highest priority
      auto p_o = priorities.front(ro);
      if (!p_o)
        continue;
      auto p = p_o.value();

      //! END RO
      me->end_ro();
      //! START RW
      RW rw(me);

      // Get the queue (collection) for that priority
      auto ret_o = jobs.get_extract(rw, p);
      if (!ret_o)
        continue;
      auto q = ret_o.value();

      if (q == nullptr) { // key not found, try again
        // force abort and re-start
        me->force_abort(rw);
        continue;
      }

      bool empty_q = false; // reset to true in dequeue() if all queues become empty
     #ifdef CHUNK_POOL
      auto p_job_o = q->dequeue_strict(rw, std::ref(empty_q), std::ref(retired_chunk));
     #else
      auto p_job_o = q->dequeue_strict(rw, std::ref(empty_q));
     #endif
      if (!p_job_o)
        continue;

      // Check if all queues are empty
      if (empty_q) {
        // Re-read the highest priority
        auto p2_o = priorities.front(rw);
        if (!p2_o) {
          continue;
        }
          
        auto p2 = p2_o.value();
        // Force-Abort (restart) if priority has changed
        if (p != p2) {
          me->force_abort(rw);
          continue;
        }

        // All queues empty -> remove the queue from the hash table of queues
        auto r_o = jobs.remove(rw, p);
        if (!r_o) {
          continue;
        }
          
        // Remove the priority from the skip list
        auto rr_o = priorities.remove_front(rw, p);
        if (!rr_o) {
          continue;
        }
      }
      
      if (!me->try_end_rw()) {
        continue;
      }

     #ifdef CHUNK_POOL
      // if needed, retire a chunk (only once we know the operation has succeeded!)
      if (retired_chunk) {
        retired_chunk->reset();
        t_local_pool->pool_insert_chunk(retired_chunk);
      }
     #endif
      return p_job_o.value();
    }
  }

  private:

  /// Insert a job into the priority queue
  ///
  /// @param me   The caller's thread context
  /// @param delta_p The delta-shifted priority (if DELTA != 0) for the new jobs
  /// @param job  The new jobs
  void insert_batch_internal(OPTSTM *me, P delta_p, std::vector<kv_t> batch, int batch_size) {
    // Since we're using OPTSTM2, we need to manually handle rollback:
    while (true) {
      RW rw(me);
      
      // Get the collection where this job should go.  If it returns nullptr,
      // the priority doesn't exist yet in the PQ, so we'll need to make a
      // collection and ultimately update the skip list.
      //
      // [mfs] This faux upsert is kind of ugly.  Redesign the API?
      auto c_o = jobs.get_ins(rw, delta_p);
      if (!c_o)
        continue;
      auto c = c_o.value();
      // If this is a new priority, then make a collection
      bool newprio = (c == nullptr);
      if (c == nullptr) {
        auto o = jobs.make_collection(rw, delta_p);
        if (!o)
          continue;
        c = o.value();
      }
      // Put the job into the collection
     #ifdef CHUNK_POOL
      bool used_pool;
      if (!c->enqueue_batch_vec(rw, *t_local_pool, used_pool, batch, batch_size)) continue;
     #else
      if (!c->enqueue_batch_vec(rw, batch, batch_size)) continue;
     #endif
      // Update the skip list, if necessary
      if (newprio && !priorities.insert_guaranteed(rw, delta_p))
        continue;
      if (!me->try_end_rw())
        continue;

     #ifdef CHUNK_POOL
      if (used_pool) t_local_pool->pool_remove_chunk();
     #ifdef PROFILING
      auto ret = t_local_pool->check_prof_fields();
      if (ret) {
        if (ret.value()) t_num_pool_use++;
        else t_num_allocs++;
      }
     #endif
     #endif

      return;
    }
  }

  public:

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
      me->end_ro();
      return res.value();
    }
  }

  /// Debug: Print the data structure
  std::pair<long,long> dump(OPTSTM *me, bool print=false) {
    RO ro(me);
    //priorities.dump(ro);
    std::pair<long,long> ret = jobs.dump(ro, print);

   #ifdef PROFILING
    if (print) std::cout << "Before thread-locals: " << "\n\t- Number of elements: " << ret.first << "\n\t- Keysum: " << ret.second << "\n";

    // parse collected local arrays
    for (int i = 0; i < collected_remove_arr.size(); i++) {
      auto max = collected_remove_arr[i]->enqueues.get(ro, collected_remove_arr[i]).value();

      for (int j = collected_num_items[i]; j < max; j++) {
        auto key = collected_remove_arr[i]->elements[j].prio.get(ro, collected_remove_arr[i]).value();
        ret.first++;
        ret.second += key;
      }
    }

    std::cout << "Total allocations: " << collected_num_allocs << "\n";
    std::cout << "Total avoided allocations (use of pool): " << collected_num_pool_use << "\n";
   #endif
    me->end_ro();
    return ret;
  }

  void dump_ht() {
    std::vector<std::pair<P,long>> ret = jobs.dump_ht();
    // sort the vector
    std::sort(ret.begin(), ret.end(), 
        [](const auto& a, const auto& b) {
            return a.first < b.first;
        }
    );

    // print sorted_prios
    for (const auto& [key, value] : ret) {
        std::cout << "[" << key << "]: " << value << std::endl;
    }
  }

  // Accumulates locally stored elements into global vectors
  // To be called by each thread before termination for keysum check
  void thread_terminate() {
    // add t_remove_arr to vector
   #ifdef PROFILING
    std::lock_guard<std::mutex> lock(global_mutex);
    if (t_remove_arr != nullptr) {
      collected_remove_arr.push_back(t_remove_arr);
      collected_num_items.push_back(t_cur_idx);
    }
   
    collected_num_allocs += t_num_allocs;
    collected_num_pool_use += t_num_pool_use;
   #endif
  }
};
