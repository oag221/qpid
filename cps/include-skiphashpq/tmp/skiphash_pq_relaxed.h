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

#include <sstream>
#include <iostream>
#include <climits>

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

class skiphash_pq_relaxed_base {
public:
  static const int S = 2; 
  static const int BUCKETS_PER_POWER = (1 << S);
  static const int S_MASK = BUCKETS_PER_POWER - 1;

  static inline int get_fine_grained_bucket(uint64_t prio) {
    if (prio == 0) return 0;

    int msb = 63 - __builtin_clzll(prio);
    
    // We want the S bits immediately BELOW the MSB.
    // Example for S=2: 
    // If prio is 10110 (binary), msb is 4. 
    // We want bits at pos 3 and 2 (the '01').
    int sub_idx = 0;
    if (msb >= S) {
        sub_idx = (prio >> (msb - S)) & S_MASK;
    } else {
        // If the number is too small to have S bits below it (like prio=1),
        // we shift it up so the bits we do have land in the right spot.
        sub_idx = (prio << (S - msb)) & S_MASK;
    }

    return (msb << S) | sub_idx;
}

};

/// A priority queue, implemented as a skip list of priorities and a hash table
/// that maps priorities to queues of tasks.
///
/// @tparam P       The type of priorities stored in this priority queue
/// @tparam J       The type of jobs stored in this priority queue
/// @tparam OPTSTM  A thread descriptor type, for safe memory reclamation
template <typename P, typename J, class OPTSTM> class skiphash_pq_relaxed : public skiphash_pq_relaxed_base {
  using RO = typename OPTSTM::RO;
  using RW = typename OPTSTM::RW;

  using SL_T = skipslist_oset<P, OPTSTM>;
  using Q_T = chunk_queue<P, J, OPTSTM>;
  using QD_T = chunk_queue_diffractor<Q_T, P, J, OPTSTM>;
  using DCL_T = dclist_omap<P, QD_T, OPTSTM>;
  using UMAP_T = caucmap_adapter<P, QD_T, OPTSTM, DCL_T>;

  using chunk_t = typename Q_T::q_node_t;

  /// Thread local pointer to thread-owned "local" node to be processed by del-mins
  inline static thread_local chunk_t* t_remove_arr = nullptr;
  inline static thread_local int t_cur_idx = 0;
  //inline static thread_local P t_cur_extract_p = -1;

  inline static thread_local chunk_t* t_cur_ins_batch = nullptr;
  inline static thread_local int t_cur_ins_delta = -1;
  
  inline static thread_local std::vector<P> t_ins_vec_prios;
  inline static thread_local std::vector<J> t_ins_vec_jobs;
  inline static thread_local std::vector<std::pair<P,J>> t_ins_vec;
  inline static thread_local int t_ins_vec_idx;

  //! For debugging
  inline static thread_local int t_change_p = 0;
  inline static thread_local long t_local_size = 0;
  inline static thread_local long t_local_size_MIN = INT_MAX;
  inline static thread_local long t_local_size_MAX = -1;
  inline static thread_local long t_local_size_cnt = 0;

  /// Used post-experiment to store thread-local data for keysum and size validations
  /// Only used once experiment has ended (end time has been read)
  std::mutex global_mutex;
  std::vector<chunk_t *> collected_remove_arr;
  std::vector<int> collected_num_items;
  long collected_change_p = 0;
  long collected_loc_size = 0;
  long collected_loc_size_cnt = 0;
  long collected_local_size_MIN = INT_MAX;
  long collected_local_size_MAX = -1;


  /// Global data structures
  UMAP_T jobs;     // The unordered map of queues of jobs
  SL_T priorities; // The skip list of priorities

  const int MAX_BATCH_SIZE;
  const int CHUNK_SIZE;
  const int DELTA;

public:
  template <typename config_t>
  skiphash_pq_relaxed(OPTSTM *me, config_t *cfg) :
    jobs(me, cfg),
    priorities(me, cfg),
    MAX_BATCH_SIZE(cfg->max_batch_size),
    CHUNK_SIZE(cfg->chunksize),
    DELTA(cfg->delta) {}

  void init_thread() {
    t_remove_arr = nullptr;
    t_cur_idx = 0;
    //t_cur_extract_p = -1;
    t_cur_ins_batch = nullptr;
    t_cur_ins_delta = -1;
    // t_ins_vec_prios.resize(MAX_BATCH_SIZE + 1);
    // t_ins_vec_jobs.resize(MAX_BATCH_SIZE + 1);
    t_ins_vec.resize(MAX_BATCH_SIZE + 1);
    t_ins_vec_idx = 0;
    t_local_size = 0;
    t_local_size_cnt = 0;
  }

  // uint64_t get_log_bucket(int prio) {
  //     if (prio <= 0) return 0;
  //     // __builtin_clz returns leading zeros. 
  //     // 31 - clz gives the index of the highest set bit.
  //     // Example: 1024 (2^10) returns bucket 10.
  //     return 31 - __builtin_clz(prio);
  // }

  int get_hybrid_bucket(int prio) {
      if (prio <= 0) return 0;
      return 31 - __builtin_clz(prio);
  }

  int get_relaxed_priority(int bucket_idx) {
    return (1 << bucket_idx); 
  }

  void pre_insert(OPTSTM *me, int range) {
    int inc = 50;
    for (int i = 0; i < range; i+=inc) {
      if (i && i % 500 == 0) {
        std::cout << "Inserted " << i << " collections...\n";
      }
      int idx = 0;
      while (true) {
        RW rw(me);
        while (idx < inc) {
          auto o = jobs.make_collection(rw, i + idx);
          idx++;
        }
        if (me->try_end_rw())
          break;
        idx = 0;
      }
    }
  }

  /// Insert a job into the priority queue
  ///
  /// @param me   The caller's thread context
  /// @param prio The priority for the new job
  /// @param job  The new job
  /// ! API !
  void insert(OPTSTM *me, P prio, J job) {
  //  #ifdef PAGE_RANK
  //   auto delta_p = prio;
  //  #else

    #ifdef LOG_BUCKETS
    auto delta_p = get_fine_grained_bucket(prio);
    #else
    auto delta_p = prio >> DELTA;
    #endif
  //  #endif
    // Since we're using OPTSTM2, we need to manually handle rollback:
    while (true) {
      RW rw(me);
      
      // Get the collection where this job should go.  If it returns nullptr,
      // the priority doesn't exist yet in the PQ, so we'll need to make a
      // collection and ultimately update the skip list.
      //
      // [mfs] This faux upsert is kind of ugly.  Redesign the API?

      auto c_o = jobs.get_ins(rw, delta_p);
      //auto c_o = jobs.get_ins(rw, delta_p);
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
      if (!c->enqueue(rw, prio, job))
        continue;
      // Update the skip list, if necessary
      //!todo: also need to update SL if collection was empty before (under new change...)
      if (newprio && !priorities.insert_guaranteed(rw, delta_p))
        continue;
      if (!me->try_end_rw())
        continue;
      return;
    }
  }

  // Batch - Implementation 1
  /// ! API !
  void insert_vec(OPTSTM *me, P prio, J job, bool ordered=false) {
   #ifdef LOG_BUCKETS
    t_ins_vec[t_ins_vec_idx].first = prio;
   #else
    t_ins_vec[t_ins_vec_idx].first = (prio >> DELTA);
   #endif
    
    t_ins_vec[t_ins_vec_idx++].second = job;
    flush_vec(me, false, ordered);
  }

  // Batch - Implementation 1
  inline void flush_vec(OPTSTM *me, bool force=false, bool ordered=false) {
    // flush_vec_fewer_TX(me, force); //! hasn't been updated with delta stuff idt
    flush_vec_internal(me, force, ordered);
  }

/*
  void sortJByP() {
    if (t_ins_vec_idx <= 1) return;

    uint16_t p[MAX_BATCH_SIZE];

    for (size_t i = 0; i < t_ins_vec_idx; ++i) {
      p[i] = static_cast<uint16_t>(i);
    }

    std::sort(p, p + t_ins_vec_idx, [&](uint8_t i, uint8_t j) {
        return t_ins_vec_prios[i] < t_ins_vec_prios[j];
    });

    for (size_t i = 0; i < t_ins_vec_idx; ++i) {
        // While the element at i is not the one that belongs there
        // We chase the cycle until it is resolved.
        while (p[i] != i) {
            size_t target = p[i];

            // swap items in both
            std::swap(t_ins_vec_prios[i], t_ins_vec_prios[target]);
            std::swap(t_ins_vec_jobs[i], t_ins_vec_jobs[target]);
            
            // Swap the permutation indices to mark target as "done" (implicitly)
            // and bring the next target into the current slot (p[i])
            std::swap(p[i], p[target]);
        }
    }
  }
*/

  void flush_vec_internal(OPTSTM *me, bool force=false, bool ordered=false) {
    if ((t_ins_vec_idx == MAX_BATCH_SIZE) || (force && t_ins_vec_idx)) {
      
      // If indicated, sort both vectors
      if (ordered) std::sort(t_ins_vec.begin(), t_ins_vec.end());

      // FLUSH
      int cur_idx = 0;
      while (cur_idx < t_ins_vec_idx) {
        int start_idx = cur_idx;

       #ifdef LOG_BUCKETS
        auto cur_log_p = get_fine_grained_bucket(t_ins_vec[cur_idx].first);
        auto prev_p = cur_log_p;
        while ((cur_idx < t_ins_vec_idx) && (prev_p == cur_log_p)) {
          cur_log_p = get_fine_grained_bucket(t_ins_vec[++cur_idx].first);
        }
       #else
        auto cur_delt_p = t_ins_vec[cur_idx].first >> DELTA;
        auto prev_p = cur_delt_p;
        while ((cur_idx < t_ins_vec_idx) && (prev_p == cur_delt_p)) {
          cur_delt_p = t_ins_vec[++cur_idx].first >> DELTA;
        }
       #endif

        // insert [start_idx:cur_idx]
        std::vector<std::pair<P,J>> vec_sub(t_ins_vec.begin() + start_idx, t_ins_vec.begin() + cur_idx);
        insert_batch_vec(me, prev_p, vec_sub, cur_idx - start_idx);
      }
      t_ins_vec_idx = 0;
    }
  }

  void flush_vec_fewer_TX(OPTSTM *me, bool force=false) {
    // Flush if either (1) vector is full, or (2) `force` is specified and vector is non-empty
    if ((t_ins_vec_idx == MAX_BATCH_SIZE) || (force && t_ins_vec_idx)) {
      // FLUSH
      const int max_per_tx = 3; // max batch-inserts to perform per transaction
      int glob_cur_idx = 0;
      while (glob_cur_idx < t_ins_vec_idx) {
        int cur_num_ops = 0;
        
        while (true) {
          bool abort = false;
          int tmp_cur_idx = glob_cur_idx;
          RW rw(me);
          
          // perform up to `max_per_tx` operations before completing the transaction
          while (tmp_cur_idx < t_ins_vec_idx && cur_num_ops < max_per_tx) {
            int start_idx = tmp_cur_idx;
            auto cur_delt_p = t_ins_vec[tmp_cur_idx].first;
            auto prev_delta_p = cur_delt_p;
            while ((tmp_cur_idx < t_ins_vec_idx) && (prev_delta_p == cur_delt_p)) {
              cur_delt_p = t_ins_vec[++tmp_cur_idx].first;
            }

            // insert [start_idx:cur_idx]
            std::vector<std::pair<P,J>> vec_sub(t_ins_vec.begin() + start_idx, t_ins_vec.begin() + tmp_cur_idx);
            if (!insert_batch_vec_oneT(rw, prev_delta_p, vec_sub, tmp_cur_idx - start_idx)) {
              abort = true;
              break;
            }
            cur_num_ops++;
          }
          if (abort) continue;

          if (me->try_end_rw()) {
            glob_cur_idx = tmp_cur_idx; // 'solidify' previous insertions
            break;
          }
        }
      }

      t_ins_vec_idx = 0;
    }
  }

  /// Insert a job into the priority queue
  /// Invariant: if t_cur_ins_batch is NOT nullptr, it has space for another element
  ///
  /// @param me   The caller's thread context
  /// @param prio The priority for the new jobs
  /// @param job  The new jobs

  // TODO: handle case in which batch_size > chunk_size (local chunk should be sized to chunk_size, next pointers used)
  // Batch - Implementation 2
  /// ! API !
  void insert_chunk(OPTSTM *me, P prio, J job) {
    auto delta_p = prio >> DELTA;
    
    // Check if need to create a new batch
    if (t_cur_ins_batch == nullptr || delta_p != t_cur_ins_delta) { // todo: what if meant for re-use ???
      if (t_cur_ins_batch != nullptr) {
        // Flush existing batch
        flush_chunk(me);
      }
      t_cur_ins_delta = delta_p;
      make_new_batch(me, job);
      return;
    }
    
    // Append to current batch
    int enqs = t_cur_ins_batch->enqueues.get_unsafe();
    t_cur_ins_batch->elements[enqs].set_unsafe(job);
    t_cur_ins_batch->enqueues.set_unsafe(enqs + 1);

    // flush if full
    if ((enqs + 1) == CHUNK_SIZE) { // TODO: handle case in which batch_size > chunk_size
      flush_chunk(me);
    }
  }

  // Batch - Implementation 2
  void flush_chunk(OPTSTM *me) {
    if (t_cur_ins_batch && t_cur_ins_batch->enqueues.get_unsafe() > 0) {
      // store initial ptr to re-set t_cur_ins_batch in case of abort
      auto initial_t_cur_ins_batch = t_cur_ins_batch;

      while (true) {
        RW rw(me);
        
        // Get the collection where this job should go.  If it returns nullptr,
        // the priority doesn't exist yet in the PQ, so we'll need to make a
        // collection and ultimately update the skip list.
        //
        // [mfs] This faux upsert is kind of ugly.  Redesign the API?
        assert(!(t_cur_ins_delta == -1));
        auto c_o = jobs.get_ins(rw, t_cur_ins_delta);
        if (!c_o)
          continue;
        auto c = c_o.value();
        // If this is a new priority, then make a collection
        bool newprio = (c == nullptr);
        if (c == nullptr) {
          auto o = jobs.make_collection(rw, t_cur_ins_delta);
          if (!o)
            continue;
          c = o.value();
        }
        // Put the job into the collection
        if (!c->enqueue_batch_chunk(rw, std::ref(t_cur_ins_batch))) { //! NOTE: this either null's t_cur_ins_batch (if inserted to global), or it is re-used
          t_cur_ins_batch = initial_t_cur_ins_batch;
          continue;
        } 
        
        // Update the skip list, if necessary
        if (newprio && !priorities.insert_guaranteed(rw, t_cur_ins_delta)) {
          t_cur_ins_batch = initial_t_cur_ins_batch;
          continue;
        }
          
        if (!me->try_end_rw()) {
          t_cur_ins_batch = initial_t_cur_ins_batch;
          continue;
        }
        break;
      }
    }
  }

  /// Get the highest priority job out of the priority queue
  /// Invariant: if t_remove_arr is not nullptr, there is at least one more element to be consumed
  ///
  /// [mfs] Are the semantics right here?  What if the queue is empty?
  ///
  /// @param me The caller's thread context
  ///
  /// @return The job that was found
  std::optional<std::pair<P,J>> extract_min(OPTSTM *me) {
    // first consult thread-local array
    if (t_remove_arr != nullptr) {
      auto ret_p = t_remove_arr->elements[t_cur_idx].prio.get_unsafe();
      auto ret_j = t_remove_arr->elements[t_cur_idx++].job.get_unsafe();
      check_reclaim(me); // check if need to reclaim t_remove_arr
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

      //std::cout << "MAX PRIO IS " << p << "\n";

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

      //t_cur_extract_p = p;
      // Pull a job out
      auto ret = std::make_pair(t_remove_arr->elements[0].prio.get_unsafe(), t_remove_arr->elements[0].job.get_unsafe());
      t_cur_idx = 1;

      // We made all queues empty
      if (empty_q) {
        std::cout << "ALL Qs EMPTY - removing collection (p=" << p << ")\n";
        // TODO: alternatively, (instead of re-reading and aborting if changed) could implement a remove_priority() from SL, better for throughput but affects relaxation (at least practically)
        // Re-read the highest priority
        auto p2_o = priorities.front(rw);
        if (!p2_o) {
          t_remove_arr = nullptr;
          continue;
        }
          
        auto p2 = p2_o.value();
        // Force-Abort (restart) if priority has changed
        if (p != p2) {
          //t_change_p++;
          t_remove_arr = nullptr;
          me->force_abort(rw);
          continue;
        }

        // All queues empty -> mark collection as empty
        //auto r_o = jobs.mark_empty(rw, p);
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

      // auto enqs = t_remove_arr->enqueues.get_unsafe();
      // t_local_size += enqs;
      // t_local_size_cnt++;
      // if (enqs < t_local_size_MIN) {
      //   t_local_size_MIN = enqs;
      // } else if (enqs > t_local_size_MAX) {
      //   t_local_size_MAX = enqs;
      // }

      // check if need to reclaim
      check_reclaim(me);
      return ret;
    }
  }

  /// Get the highest priority job out of the priority queue
  /// Invariant: if t_remove_arr is not nullptr, there is at least one more element to be consumed
  ///
  /// [mfs] Are the semantics right here?  What if the queue is empty?
  ///
  /// @param me The caller's thread context
  ///
  /// @return The job that was found
  std::optional<std::pair<P,J>> extract_min_strict(OPTSTM *me) {
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
      auto p_job_o = q->dequeue_strict(rw, empty_q);
      if (!p_job_o)
        continue;

      // Check if all queues are empty
      if (empty_q) {
        // std::cout << "COLLECTION IS EMPTY\n";
        // TODO: alternatively, (instead of re-reading and aborting if changed) could implement a remove_priority() from SL, better for throughput but affects relaxation (at least practically)
        // Re-read the highest priority
        auto p2_o = priorities.front(rw);
        if (!p2_o) {
          continue;
        }
          
        auto p2 = p2_o.value();
        // Force-Abort (restart) if priority has changed
        if (p != p2) {
          //t_change_p++;
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

      // #ifdef PAGE_RANK
      //  return std::make_pair(p, p_job_o.value());
      // #else
      
       return p_job_o.value();
       //return std::make_pair((p << DELTA), p_job_o.value());
      //#endif
      
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
      me->end_ro();
      return res.value();
    }
  }

  /// Debug: Print the data structure
  std::pair<long,long> dump(OPTSTM *me, bool print=false) {
    RO ro(me);
    //priorities.dump(ro);
    
    std::pair<long,long> ret = jobs.dump(ro, print); //!

    std::cout << "Before thread-locals: " << "\n\t- Number of elements: " << ret.first << "\n\t- Keysum: " << ret.second << "\n";

    // parse collected local arrays
    for (int i = 0; i < collected_remove_arr.size(); i++) {
      auto max = collected_remove_arr[i]->enqueues.get(ro, collected_remove_arr[i]).value();

      for (int j = collected_num_items[i]; j < max; j++) {
        auto job = collected_remove_arr[i]->elements[j].get(ro, collected_remove_arr[i]).value();
        ret.first++;
        ret.second += job;
      }
    }

    long avg_loc_size = 0;
    if (collected_loc_size_cnt) {
      avg_loc_size = collected_loc_size / collected_loc_size_cnt;
      std::cout << "Avg size of locally stored chunk: " << avg_loc_size << "\n";
      std::cout <<"\t [MAX: " << collected_local_size_MAX << "]\n";
      std::cout <<"\t [MIN: " << collected_local_size_MIN << "]\n";
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
      collected_change_p += t_change_p;
    }

    collected_loc_size += t_local_size;
    collected_loc_size_cnt += t_local_size_cnt;
    if (t_local_size_MIN < collected_local_size_MIN) {
      collected_local_size_MIN = t_local_size_MIN;
    } else if (t_local_size_MAX > collected_local_size_MAX) {
      collected_local_size_MAX = t_local_size_MAX;
    }
  }

private:

  /// Insert a job into the priority queue
  ///
  /// @param me   The caller's thread context
  /// @param delta_p The delta-shifted priority (if DELTA != 0) for the new jobs
  /// @param job  The new jobs
  std::optional<bool> insert_batch_vec_oneT(RW &rw, P delta_p, std::vector<J> batch, int batch_size) {
    // Get the collection where this job should go.  If it returns nullptr,
    // the priority doesn't exist yet in the PQ, so we'll need to make a
    // collection and ultimately update the skip list.
    //
    // [mfs] This faux upsert is kind of ugly.  Redesign the API?
    auto c_o = jobs.get_ins(rw, delta_p);
    if (!c_o) return {}; // ABORT!
    auto c = c_o.value();

    // If this is a new priority, then make a collection
    bool newprio = (c == nullptr);
    if (c == nullptr) {
      auto o = jobs.make_collection(rw, delta_p);
      if (!o) return {}; // ABORT!
      c = o.value();
    }

    // Put the job into the collection
    if (!c->enqueue_batch_vec(rw, batch, batch_size)) return {}; // ABORT!
    // Update the skip list, if necessary
    if (newprio && !priorities.insert_guaranteed(rw, delta_p)) return {}; // ABORT!
    
    return true;
  }

  /// Insert a job into the priority queue
  ///
  /// @param me   The caller's thread context
  /// @param delta_p The delta-shifted priority (if DELTA != 0) for the new jobs
  /// @param job  The new jobs
  void insert_batch_vec(OPTSTM *me, P delta_p, std::vector<std::pair<P,J>> batch, int batch_size) {
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
      if (!c->enqueue_batch_vec(rw, batch, batch_size))
        continue;
      // Update the skip list, if necessary
      if (newprio && !priorities.insert_guaranteed(rw, delta_p))
        continue;
      if (!me->try_end_rw())
        continue;
      return;
    }
  }

  /// Check if t_remove_arr needs to be reclaimed (i.e., we have exhausted all items in the node with extract_min() calls)
  ///
  /// @param rw A read/write transactional context
  void check_reclaim(OPTSTM *me, bool debug=false, bool last=false) {
    auto num_items = t_remove_arr->enqueues.get_unsafe();

    if (t_cur_idx == num_items) {
      while (true) {
        RW rw(me);
        rw.reclaim(t_remove_arr);
        if (me->try_end_rw())
          break;
      }
      t_remove_arr = nullptr;
    }
  }

  /// Make a new batch and insert a new element to it
  ///
  /// @param me The STM instance
  /// @param job The job to insert
  void make_new_batch(OPTSTM *me, J& job) {
    while (true) {
      RW rw(me);

      // Create and add new job to buffer
      chunk_t* new_batch = chunk_t::make_node(rw, CHUNK_SIZE);
      new_batch->elements[0].set_cap(rw, new_batch, job);
      new_batch->enqueues.set_cap(rw, new_batch, 1);

      if (me->try_end_rw()) {
        t_cur_ins_batch = new_batch;
        return;
      }
    }
  }
};
