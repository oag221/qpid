/*
 * This file belongs to the Galois project, a C++ library for exploiting
 * parallelism. The code is being released under the terms of the 3-Clause BSD
 * License (a copy is located in LICENSE.txt at the top-level directory).
 *
 * Copyright (C) 2018, The University of Texas at Austin. All rights reserved.
 * UNIVERSITY EXPRESSLY DISCLAIMS ANY AND ALL WARRANTIES CONCERNING THIS
 * SOFTWARE AND DOCUMENTATION, INCLUDING ANY WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR ANY PARTICULAR PURPOSE, NON-INFRINGEMENT AND WARRANTIES OF
 * PERFORMANCE, AND ANY WARRANTY THAT MIGHT OTHERWISE ARISE FROM COURSE OF
 * DEALING OR USAGE OF TRADE.  NO WARRANTY IS EITHER EXPRESS OR IMPLIED WITH
 * RESPECT TO THE USE OF THE SOFTWARE OR DOCUMENTATION. Under no circumstances
 * shall University be liable for incidental, special, indirect, direct or
 * consequential damages or loss of profits, interruption of business, or
 * related expenses which may arise from use of Software or Documentation,
 * including but not limited to those resulting from defects in Software and/or
 * Documentation, or loss or inaccuracy of data of any kind.
 */

#include "galois/Galois.h"
#include "galois/AtomicHelpers.h"
#include "galois/gstl.h"
#include "galois/Reduction.h"
#include "galois/PriorityQueue.h"
#include "galois/Timer.h"
#include "galois/graphs/LCGraph.h"
#include "galois/graphs/TypeTraits.h"
#include "Lonestar/BoilerPlate.h"
#include "Lonestar/BFS_SSSP.h"
#include "Lonestar/Utils.h"

#include <include/MultiBucketQueue.h>
#include <include/MultiQueue.h>

#include "llvm/Support/CommandLine.h"

#include "../config.h"
#include <include-skiphashpq/optstm2/eager_noext_c1.h>
#include <include-skiphashpq/skiphash_pq_relaxed.h>

#include <include-pipq/pipq_impl.h>

#include <include-smq/smq_impl.h>

#include <include-linden/linden.h>
#include "gc/ptst.h"

#include "intset.h"
#include <common/include/random.h>

#include "../termination_helper.h"

#include <iostream>

OPTSTM2_GLOBALS_INITIALIZER;

extern termination_detector_2 detector;

const int pinning[96] = {0,2,4,6,8,10,12,14,16,18,20,22,24,26,28,30,32,34,36,38,40,42,44,46,1,3,5,7,9,11,13,15,17,19,21,23,25,27,29,31,33,35,37,39,41,43,45,47,48,50,52,54,56,58,60,62,64,66,68,70,72,74,76,78,80,82,84,86,88,90,92,94,49,51,53,55,57,59,61,63,65,67,69,71,73,75,77,79,81,83,85,87,89,91,93,95};

alignas(64) extern uint8_t levelmax[64];
__thread unsigned long *seeds;

namespace cll = llvm::cl;

static const char* name = "Single Source Shortest Path";
static const char* desc =
    "Computes the shortest path from a source node to all nodes in a directed "
    "graph using a modified chaotic iteration algorithm";
static const char* url = "single_source_shortest_path";

static cll::opt<std::string>
    inputFile(cll::Positional, cll::desc("<input file>"), cll::Required);
static cll::opt<unsigned int>
    startNode("startNode",
              cll::desc("Node to start search from (default value 0)"),
              cll::init(0));
static cll::opt<unsigned int>
    destNode("destNode",
              cll::desc("Node to end search from (default value 0)"),
              cll::init(0));
static cll::opt<unsigned int>
    reportNode("reportNode",
               cll::desc("Node to report distance to(default value 1)"),
               cll::init(1));
static cll::opt<unsigned int>
    stepShift("delta",
              cll::desc("Shift value for the deltastep (default value 13)"),
              cll::init(13));
static cll::opt<unsigned int>
    threadNum("threads",
              cll::desc("number of threads for MQ"),
              cll::init(1));
static cll::opt<unsigned int>
    queueNum("queues",
              cll::desc("number of queues for MQ"),
              cll::init(4));
static cll::opt<unsigned int>
    bucketNum("bucketNum",
              cll::desc("number of buckets"),
              cll::init(64));
static cll::opt<unsigned int>
    batch1("batch1",
              cll::desc("bucketing batch size"),
              cll::init(1));
static cll::opt<unsigned int>
    batch2("batch2",
              cll::desc("bucketing batch size"),
              cll::init(1));
static cll::opt<unsigned int>
    chunk("chunk",
              cll::desc("chunk size for tuning"),
              cll::init(64));
static cll::opt<unsigned int>
    stickiness("stick",
              cll::desc("stickiness"),
              cll::init(1));
static cll::opt<unsigned int>
    prefetch("prefetch",
              cll::desc("prefetching"),
              cll::init(0));

// For SkipHashPQ
static cll::opt<unsigned int>
    chunksize("chunksize",
        cll::desc("Size of each chunk"),
        cll::init(128));
static cll::opt<unsigned int>
    num_chunks("num_chunks",
        cll::desc("Number of queues"),
        cll::init(4));
static cll::opt<unsigned int>
    sl_max_levels("sl_max_levels",
        cll::desc("Skiplist max levels"),
        cll::init(32));
static cll::opt<unsigned int>
    buckets("buckets",
        cll::desc("Number of buckets in SL"),
        cll::init(1048576));
static cll::opt<unsigned int>
    batch("batch",
        cll::desc("Whether or not SkipHashPQ should use a batch"),
        cll::init(0));
static cll::opt<unsigned int>
    max_batch_size("max_batch_size",
        cll::desc("Max size of insert batch"),
        cll::init(64));

static cll::opt<unsigned int>
    order_batch("order_batch",
        cll::desc("Whether or not to order the batch"),
        cll::init(0));
static cll::opt<unsigned int>
    strict("strict",
        cll::desc("Whether or not to use strict"),
        cll::init(0));

static cll::opt<unsigned int>
    steal_prob("steal_prob",
        cll::desc("Whether or not to use strict"),
        cll::init(8));
static cll::opt<unsigned int>
    steal_size("steal_size",
        cll::desc("Whether or not to use strict"),
        cll::init(8));

enum Algo {
  deltaStepOBIM,
  deltaStepPMOD,
  MQ,
  MQBucket,
  SkipHashPQ,
  PIPQ,
  SMQ,
  Linden,
  Spray
};

const char* const ALGO_NAMES[] = {
    "deltaStepOBIM","deltaStepPMOD", "MQ","MQBucket", "SkipHashPQ", "PIPQ", "SMQ", "Linden", "Spray"};

static cll::opt<Algo> algo(
    "algo", cll::desc("Choose an algorithm (default value auto):"),
    cll::values(clEnumVal(deltaStepOBIM, "deltaStepOBIM"),
                clEnumVal(deltaStepPMOD, "deltaStepPMOD"),
                clEnumVal(MQ, "MQ"),
                clEnumVal(MQBucket, "MQBucket"),
                clEnumVal(SkipHashPQ, "SkipHashPQ"),
                clEnumVal(PIPQ, "PIPQ"),
                clEnumVal(SMQ, "SMQ"),
                clEnumVal(Linden, "Linden"),
                clEnumVal(Spray, "Spray")));

//! [withnumaalloc]
using Graph = galois::graphs::LC_CSR_Graph<std::atomic<uint32_t>, uint32_t>::
    with_no_lockable<true>::type ::with_numa_alloc<true>::type;
//! [withnumaalloc]
typedef Graph::GraphNode GNode;

constexpr static const bool TRACK_WORK          = true;
constexpr static const unsigned CHUNK_SIZE      = 64U;
constexpr static const ptrdiff_t EDGE_TILE_SIZE = 512;

using SSSP                 = BFS_SSSP<Graph, uint32_t, true, EDGE_TILE_SIZE>;
using Dist                 = SSSP::Dist;
using UpdateRequest        = SSSP::UpdateRequest;
using UpdateRequestIndexer = SSSP::UpdateRequestIndexer;
using ReqPushWrap          = SSSP::ReqPushWrap;
using OutEdgeRangeFn       = SSSP::OutEdgeRangeFn;

template <typename T, typename OBIMTy, typename P, typename R>
uint32_t deltaStepAlgo(Graph& graph, GNode source, const P& pushWrap,
                   const R& edgeRange) {

  //! [reducible for self-defined stats]
  galois::GAccumulator<size_t> BadWork;
  //! [reducible for self-defined stats]
  galois::GAccumulator<size_t> WLEmptyWork;

  graph.getData(source) = 0;

	// queue?
  galois::InsertBag<T> initBag;

	// push the source node into the queue
  pushWrap(initBag, source, 0, "parallel");

	// for_each calls for_each_gen()
	// which takes in a rangemaker, operator function and other arguments
	// for_each_gen() calls for_each_impl()
	// initializes the ForEachExecutor,taking in the 
	// worklist based on the rangemaker, then 
	// calls threadpool to run
	// init_thread() called in threadpool::run()
	// get_trait_value<wl_tag>

  auto begin = std::chrono::high_resolution_clock::now();

  galois::for_each(
      galois::iterate(initBag), // range maker
			// function to run
      [&](const T& item, auto& ctx) {
        constexpr galois::MethodFlag flag = galois::MethodFlag::UNPROTECTED;
        const auto& sdata                 = graph.getData(item.src, flag);

        if (sdata < item.dist) {
          if (TRACK_WORK)
            WLEmptyWork += 1;
          return;
        }

        auto& destDist = graph.getData(destNode, flag);
        for (auto ii : edgeRange(item)) {

          GNode dst          = graph.getEdgeDst(ii);
          auto& ddist        = graph.getData(dst, flag);
          Dist ew            = graph.getEdgeData(ii, flag);
          const Dist newDist = sdata + ew;
          if (destDist != SSSP::DIST_INFINITY && newDist > destDist)
            continue;
          Dist oldDist       = galois::atomicMin<uint32_t>(ddist, newDist);
          if (newDist < oldDist) {
            if (TRACK_WORK) {
              //! [per-thread contribution of self-defined stats]
              if (oldDist != SSSP::DIST_INFINITY) {
                BadWork += 1;
              }
              //! [per-thread contribution of self-defined stats]
            }
            pushWrap(ctx, dst, newDist);
          }
        }
      },

			// arguments
      galois::wl<OBIMTy>(UpdateRequestIndexer{stepShift}), // OBIM worklist

			// other settings
      galois::disable_conflict_detection(), galois::loopname("SSSP"));

  auto end = std::chrono::high_resolution_clock::now();
  auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(end-begin).count();
  std::cout << "runtime_ms " << ms << "\n";

  if (TRACK_WORK) {
    //! [report self-defined stats]
    galois::runtime::reportStat_Single("SSSP", "BadWork", BadWork.reduce());
    //! [report self-defined stats]
    galois::runtime::reportStat_Single("SSSP", "WLEmptyWork",
                                       WLEmptyWork.reduce());
  }

  auto& destDist = graph.getData(destNode, galois::MethodFlag::UNPROTECTED);
  return destDist;
}

using PQElement = std::tuple<uint32_t, uint32_t>;
struct stat_t {
  uint64_t iter = 0;
  uint64_t emptyWork = 0;
};

uint32_t __attribute__ ((noinline)) getPrioData(std::atomic<uint32_t> *p) {
  return p->load(std::memory_order_acquire);
}

#ifdef PERF
bool __attribute__ ((noinline)) changeMin(
#else
inline bool changeMin(
#endif
  std::atomic<uint32_t> *prios, uint32_t dst, uint32_t oldDist, uint32_t newDist) {
    uint32_t d = oldDist;
    bool swapped = false;
    do {
      if (d <= newDist) break;
      swapped = prios[dst].compare_exchange_weak(
          d, newDist,
          std::memory_order_acq_rel,
          std::memory_order_acquire);
    } while(!swapped);
    return swapped;
}

template<typename MQ, typename descriptor>
void ThreadTaskSTM(Graph& graph, MQ &wl, stat_t *stats, std::atomic<uint32_t> *prios, termination_detector_2& detector, int tid) {
  uint64_t iter = 0UL;
  uint64_t emptyWork = 0UL;
  uint dist;
  GNode src;
  auto *me = new descriptor();
  using extract_ret_t = std::optional<std::pair<uint,GNode>>;
  auto call_extract = [&]() {
    me->op_begin();
    auto ret = wl.extract_min(me); 
    me->op_end();
    return ret;
  };
  if (tid) wl.init_thread(me, tid);
  else wl.re_init_thread(me);

  while (true) {
    auto ret_term = try_extract<extract_ret_t>(detector, call_extract); // handles termination detection
    if (!ret_term) break; // TERMINATE
    std::tie(dist, src) = ret_term.value();

#ifdef PERF
    uint32_t srcD = getPrioData(&prios[src]);
#else
    uint32_t srcD = prios[src].load(std::memory_order_acquire);
#endif
    ++iter;
    if (srcD < dist) {
      // This filters out moot tasks when the vertex
      // being popped is given a lower distance
      emptyWork++;
      continue;
    }

#ifdef PERF
    uint32_t destDist = getPrioData(&prios[destNode]);
#else
    uint32_t destDist = prios[destNode].load(std::memory_order_acquire);
#endif

    // Iterate neighbors and see if their distances can be lowered
    auto edgeRange = graph.edges(src, galois::MethodFlag::UNPROTECTED);
    for (auto e : edgeRange) {
      GNode dst   = graph.getEdgeDst(e);
      const auto newDist = srcD + graph.getEdgeData(e);

      // Filter out paths that are already longer than
      // the destination node's distance.
      // They won't ever contribute to lowering the destNode's distance.
      if (destDist != UINT32_MAX && newDist > destDist) continue;
#ifdef PERF
      uint32_t oldDist = getPrioData(&prios[dst]);
#else
      uint32_t oldDist = prios[dst].load(std::memory_order_relaxed);
#endif
      // Attempt to CAS the neighbor to a lower distance
      if (changeMin(prios, dst, oldDist, newDist)) {
        me->op_begin();
        wl.insert(me, newDist, dst);
        me->op_end();
      }
    }
  }
  stats->iter = iter;
  stats->emptyWork = emptyWork;
}

template<typename MQ, typename descriptor>
void ThreadTaskSTM_Strict(Graph& graph, MQ &wl, stat_t *stats, std::atomic<uint32_t> *prios, termination_detector_2& detector, int tid) {
  uint64_t iter = 0UL;
  uint64_t emptyWork = 0UL;
  uint dist;
  GNode src;
  auto *me = new descriptor();
  if (tid) wl.init_thread(me, tid);
  else wl.re_init_thread(me);
  
  using extract_ret_t = std::optional<std::pair<uint,GNode>>;
  auto call_extract = [&]() {
    me->op_begin();
    auto ret = wl.extract_min_strict(me); 
    me->op_end();
    return ret;
  };

  while (true) {
    auto ret_term = try_extract<extract_ret_t>(detector, call_extract); // handles termination detection
    if (!ret_term) break; // TERMINATE
    std::tie(dist, src) = ret_term.value();

#ifdef PERF
    uint32_t srcD = getPrioData(&prios[src]);
#else
    uint32_t srcD = prios[src].load(std::memory_order_acquire);
#endif
    ++iter;
    if (srcD < dist) {
      // This filters out moot tasks when the vertex
      // being popped is given a lower distance
      emptyWork++;
      continue;
    }

#ifdef PERF
    uint32_t destDist = getPrioData(&prios[destNode]);
#else
    uint32_t destDist = prios[destNode].load(std::memory_order_acquire);
#endif

    // Iterate neighbors and see if their distances can be lowered
    auto edgeRange = graph.edges(src, galois::MethodFlag::UNPROTECTED);
    for (auto e : edgeRange) {
      GNode dst   = graph.getEdgeDst(e);
      const auto newDist = srcD + graph.getEdgeData(e);

      // Filter out paths that are already longer than
      // the destination node's distance.
      // They won't ever contribute to lowering the destNode's distance.
      if (destDist != UINT32_MAX && newDist > destDist) continue;
#ifdef PERF
      uint32_t oldDist = getPrioData(&prios[dst]);
#else
      uint32_t oldDist = prios[dst].load(std::memory_order_relaxed);
#endif
      // Attempt to CAS the neighbor to a lower distance
      if (changeMin(prios, dst, oldDist, newDist)) {
        me->op_begin();
        wl.insert(me, newDist, dst);
        me->op_end();
      }
    }
  }
  stats->iter = iter;
  stats->emptyWork = emptyWork;
}

template<typename MQ, typename descriptor>
void ThreadTaskSTMBatch_1(Graph& graph, MQ &wl, stat_t *stats, std::atomic<uint32_t> *prios, termination_detector_2& detector, int tid) {
  uint64_t iter = 0UL;
  uint64_t emptyWork = 0UL;
  uint dist;
  GNode src;

  auto *me = new descriptor();
  if (tid) wl.init_thread(me, tid);
  else wl.re_init_thread(me);
  
  using extract_ret_t = std::optional<std::pair<uint,GNode>>;
  auto call_extract = [&]() {
    me->op_begin();
    auto ret = wl.extract_min(me); 
    me->op_end();
    return ret;
  };

  while (true) {
    auto ret_term = try_extract<extract_ret_t>(detector, call_extract); // handles termination detection
    if (!ret_term) break; // TERMINATE
    std::tie(dist, src) = ret_term.value();

#ifdef PERF
    uint32_t srcD = getPrioData(&prios[src]);
#else
    uint32_t srcD = prios[src].load(std::memory_order_acquire);
#endif
    ++iter;
    if (srcD < dist) {
      // This filters out moot tasks when the vertex
      // being popped is given a lower distance
      emptyWork++;
      continue;
    }

#ifdef PERF
    uint32_t destDist = getPrioData(&prios[destNode]);
#else
    uint32_t destDist = prios[destNode].load(std::memory_order_acquire);
#endif

    long batch_idx = 0;
    long prev_dist = -1;
    uint32_t newDist;
    // Iterate neighbors and see if their distances can be lowered
    auto edgeRange = graph.edges(src, galois::MethodFlag::UNPROTECTED);
    for (auto e : edgeRange) {
      GNode dst   = graph.getEdgeDst(e);
      const auto newDist = srcD + graph.getEdgeData(e);

      // Filter out paths that are already longer than
      // the destination node's distance.
      // They won't ever contribute to lowering the destNode's distance.
      if (destDist != UINT32_MAX && newDist > destDist) continue;
#ifdef PERF
      uint32_t oldDist = getPrioData(&prios[dst]);
#else
      uint32_t oldDist = prios[dst].load(std::memory_order_relaxed);
#endif
      // Attempt to CAS the neighbor to a lower distance
      if (changeMin(prios, dst, oldDist, newDist)) {
        me->op_begin();
        wl.insert_batch(me, newDist, dst);
        me->op_end();
      }
    }
    me->op_begin();
    wl.flush_batch(me, true);
    me->op_end();
  }
  stats->iter = iter;
  stats->emptyWork = emptyWork;
}

template<typename MQ, typename descriptor>
void ThreadTaskSTMBatch_1_ordered(Graph& graph, MQ &wl, stat_t *stats, std::atomic<uint32_t> *prios, termination_detector_2& detector, int tid) {
  uint64_t iter = 0UL;
  uint64_t emptyWork = 0UL;
  uint dist;
  GNode src;

  auto *me = new descriptor();
  if (tid) wl.init_thread(me, tid);
  else wl.re_init_thread(me);
  
  using extract_ret_t = std::optional<std::pair<uint,GNode>>;
  auto call_extract = [&]() {
    me->op_begin();
    auto ret = wl.extract_min(me); 
    me->op_end();
    return ret;
  };

  while (true) {
    auto ret_term = try_extract<extract_ret_t>(detector, call_extract); // handles termination detection
    if (!ret_term) break; // TERMINATE
    std::tie(dist, src) = ret_term.value();

#ifdef PERF
    uint32_t srcD = getPrioData(&prios[src]);
#else
    uint32_t srcD = prios[src].load(std::memory_order_acquire);
#endif
    ++iter;
    if (srcD < dist) {
      // This filters out moot tasks when the vertex
      // being popped is given a lower distance
      emptyWork++;
      continue;
    }

#ifdef PERF
    uint32_t destDist = getPrioData(&prios[destNode]);
#else
    uint32_t destDist = prios[destNode].load(std::memory_order_acquire);
#endif

    uint32_t newDist;
    // Iterate neighbors and see if their distances can be lowered
    auto edgeRange = graph.edges(src, galois::MethodFlag::UNPROTECTED);
    for (auto e : edgeRange) {
      GNode dst   = graph.getEdgeDst(e);
      const auto newDist = srcD + graph.getEdgeData(e);

      // Filter out paths that are already longer than
      // the destination node's distance.
      // They won't ever contribute to lowering the destNode's distance.
      if (destDist != UINT32_MAX && newDist > destDist) continue;
#ifdef PERF
      uint32_t oldDist = getPrioData(&prios[dst]);
#else
      uint32_t oldDist = prios[dst].load(std::memory_order_relaxed);
#endif
      // Attempt to CAS the neighbor to a lower distance
      if (changeMin(prios, dst, oldDist, newDist)) {
        me->op_begin();
        wl.insert_batch(me, newDist, dst, true);
        me->op_end();
      }
    }
    me->op_begin();
    wl.flush_batch(me, true, true);
    me->op_end();
  }
  stats->iter = iter;
  stats->emptyWork = emptyWork;
}

template<typename MQ>
void MQThreadTask(int tid, Graph& graph, MQ &wl, stat_t *stats, std::atomic<uint32_t> *prios, termination_detector_2& detector) {
  uint64_t iter = 0UL;
  uint64_t emptyWork = 0UL;
  uint dist;
  GNode src;
  wl.initTID();
  using extract_ret_t = boost::optional<tuple<uint,GNode>>;
  auto call_extract = [&]() {
    return wl.popInternal();
  };

  while (true) {
    auto ret_term = try_extract<extract_ret_t>(detector, call_extract); // handles termination detection
    if (!ret_term) break; // TERMINATE
    std::tie(dist, src) = ret_term.value();

#ifdef PERF
    uint32_t srcD = getPrioData(&prios[src]);
#else
    uint32_t srcD = prios[src].load(std::memory_order_acquire);
#endif
    ++iter;
    if (srcD < dist) {
      // This filters out moot tasks when the vertex
      // being popped is given a lower distance
      emptyWork++;
      continue;
    }

#ifdef PERF
    uint32_t destDist = getPrioData(&prios[destNode]);
#else
    uint32_t destDist = prios[destNode].load(std::memory_order_acquire);
#endif

    // Iterate neighbors and see if their distances can be lowered
    auto edgeRange = graph.edges(src, galois::MethodFlag::UNPROTECTED);
    for (auto e : edgeRange) {
      GNode dst   = graph.getEdgeDst(e);
      const auto newDist = srcD + graph.getEdgeData(e);

      // Filter out paths that are already longer than
      // the destination node's distance.
      // They won't ever contribute to lowering the destNode's distance.
      if (destDist != UINT32_MAX && newDist > destDist) continue;
#ifdef PERF
      uint32_t oldDist = getPrioData(&prios[dst]);
#else
      uint32_t oldDist = prios[dst].load(std::memory_order_relaxed);
#endif
      // Attempt to CAS the neighbor to a lower distance
      if (changeMin(prios, dst, oldDist, newDist)) {
        wl.push(newDist, dst);
      }
    }
  }
  stats->iter = iter;
  stats->emptyWork = emptyWork;
}

template<typename MQ>
void PIPQThreadTask(int tid, Graph& graph, MQ &wl, stat_t *stats, std::atomic<uint32_t> *prios, termination_detector_2& detector) {
  uint64_t iter = 0UL;
  uint64_t emptyWork = 0UL;
  uint dist;
  GNode src;
  if (tid) wl.init_thread(tid);
  using extract_ret_t = std::optional<std::pair<uint,GNode>>;
  auto call_extract = [&]() {
    return wl.extract_min(); 
  };

  while (true) {
    auto ret_term = try_extract<extract_ret_t>(detector, call_extract); // handles termination detection
    if (!ret_term) break; // TERMINATE
    std::tie(dist, src) = ret_term.value();

#ifdef PERF
    uint32_t srcD = getPrioData(&prios[src]);
#else
    uint32_t srcD = prios[src].load(std::memory_order_acquire);
#endif
    ++iter;
    if (srcD < dist) {
      // This filters out moot tasks when the vertex
      // being popped is given a lower distance
      emptyWork++;
      continue;
    }

#ifdef PERF
    uint32_t destDist = getPrioData(&prios[destNode]);
#else
    uint32_t destDist = prios[destNode].load(std::memory_order_acquire);
#endif

    // Iterate neighbors and see if their distances can be lowered
    auto edgeRange = graph.edges(src, galois::MethodFlag::UNPROTECTED);
    for (auto e : edgeRange) {
      GNode dst   = graph.getEdgeDst(e);
      const auto newDist = srcD + graph.getEdgeData(e);

      // Filter out paths that are already longer than
      // the destination node's distance.
      // They won't ever contribute to lowering the destNode's distance.
      if (destDist != UINT32_MAX && newDist > destDist) continue;
#ifdef PERF
      uint32_t oldDist = getPrioData(&prios[dst]);
#else
      uint32_t oldDist = prios[dst].load(std::memory_order_relaxed);
#endif
      // Attempt to CAS the neighbor to a lower distance
      if (changeMin(prios, dst, oldDist, newDist)) {
        wl.push(newDist, dst);
      }
    }
  }
  stats->iter = iter;
  stats->emptyWork = emptyWork;
}

template<typename MQ>
void SMQThreadTask(int tid, Graph& graph, MQ &wl, stat_t *stats, std::atomic<uint32_t> *prios, termination_detector_2& detector) {
  uint64_t iter = 0UL;
  uint64_t emptyWork = 0UL;
  uint dist;
  GNode src;
  using extract_ret_t = std::optional<std::pair<uint,GNode>>;
  auto call_extract = [&]() {
    return wl.extract_min();
  };
  
  if (tid) wl.init_thread(tid); // tid == 0 already init to insert the source

  while (true) {
    auto ret_term = try_extract<extract_ret_t>(detector, call_extract); // handles termination detection
    if (!ret_term) break; // TERMINATE
    std::tie(dist, src) = ret_term.value();

#ifdef PERF
    uint32_t srcD = getPrioData(&prios[src]);
#else
    uint32_t srcD = prios[src].load(std::memory_order_acquire);
#endif
    ++iter;
    if (srcD < dist) {
      // This filters out moot tasks when the vertex
      // being popped is given a lower distance
      emptyWork++;
      continue;
    }

#ifdef PERF
    uint32_t destDist = getPrioData(&prios[destNode]);
#else
    uint32_t destDist = prios[destNode].load(std::memory_order_acquire);
#endif

    // Iterate neighbors and see if their distances can be lowered
    auto edgeRange = graph.edges(src, galois::MethodFlag::UNPROTECTED);
    for (auto e : edgeRange) {
      GNode dst   = graph.getEdgeDst(e);
      const auto newDist = srcD + graph.getEdgeData(e);

      // Filter out paths that are already longer than
      // the destination node's distance.
      // They won't ever contribute to lowering the destNode's distance.
      if (destDist != UINT32_MAX && newDist > destDist) continue;
#ifdef PERF
      uint32_t oldDist = getPrioData(&prios[dst]);
#else
      uint32_t oldDist = prios[dst].load(std::memory_order_relaxed);
#endif
      // Attempt to CAS the neighbor to a lower distance
      if (changeMin(prios, dst, oldDist, newDist)) {
        wl.insert(newDist, dst);
      }
    }
  }
  stats->iter = iter;
  stats->emptyWork = emptyWork;
}

template<typename MQ>
void ThreadTaskLinden(int tid, Graph& graph, MQ &wl, stat_t *stats, std::atomic<uint32_t> *prios, termination_detector_2& detector) {
  uint64_t iter = 0UL;
  uint64_t emptyWork = 0UL;
  uint dist;
  GNode src;
  using extract_ret_t = std::optional<std::pair<uint,GNode>>;
  auto call_extract = [&]() {
    return extract_min(wl);
  };

  while (true) {
    auto ret_term = try_extract<extract_ret_t>(detector, call_extract); // handles termination detection
    if (!ret_term) break; // TERMINATE
    std::tie(dist, src) = ret_term.value();

#ifdef PERF
    uint32_t srcD = getPrioData(&prios[src]);
#else
    uint32_t srcD = prios[src].load(std::memory_order_acquire);
#endif
    ++iter;
    if (srcD < dist) {
      // This filters out moot tasks when the vertex
      // being popped is given a lower distance
      emptyWork++;
      continue;
    }

#ifdef PERF
    uint32_t destDist = getPrioData(&prios[destNode]);
#else
    uint32_t destDist = prios[destNode].load(std::memory_order_acquire);
#endif

    // Iterate neighbors and see if their distances can be lowered
    auto edgeRange = graph.edges(src, galois::MethodFlag::UNPROTECTED);
    for (auto e : edgeRange) {
      GNode dst   = graph.getEdgeDst(e);
      const auto newDist = srcD + graph.getEdgeData(e);

      // Filter out paths that are already longer than
      // the destination node's distance.
      // They won't ever contribute to lowering the destNode's distance.
      if (destDist != UINT32_MAX && newDist > destDist) continue;
#ifdef PERF
      uint32_t oldDist = getPrioData(&prios[dst]);
#else
      uint32_t oldDist = prios[dst].load(std::memory_order_relaxed);
#endif
      // Attempt to CAS the neighbor to a lower distance
      if (changeMin(prios, dst, oldDist, newDist)) {
        insert(wl, newDist, dst);
      }
    }
  }
  stats->iter = iter;
  stats->emptyWork = emptyWork;
}

template<typename MQ>
void ThreadTaskSpray(int tid, Graph& graph, MQ &wl, stat_t *stats, std::atomic<uint32_t> *prios, termination_detector_2& detector, thread_data_t *data) {
  uint64_t iter = 0UL;
  uint64_t emptyWork = 0UL;
  uint dist;
  GNode src;
  thread_data_t* data_item = &(data[tid]);
  if (tid) {
    seeds = seed_rand();
  }
  using extract_ret_t = std::optional<std::pair<uint,GNode>>;
  auto call_extract = [&]() {
    return spray_delete_min_key(wl, data_item);
  };

  while (true) {
    auto ret_term = try_extract<extract_ret_t>(detector, call_extract); // handles termination detection
    if (!ret_term) break; // TERMINATE
    std::tie(dist, src) = ret_term.value();

#ifdef PERF
    uint32_t srcD = getPrioData(&prios[src]);
#else
    uint32_t srcD = prios[src].load(std::memory_order_acquire);
#endif
    ++iter;
    if (srcD < dist) {
      // This filters out moot tasks when the vertex
      // being popped is given a lower distance
      emptyWork++;
      continue;
    }

#ifdef PERF
    uint32_t destDist = getPrioData(&prios[destNode]);
#else
    uint32_t destDist = prios[destNode].load(std::memory_order_acquire);
#endif

    // Iterate neighbors and see if their distances can be lowered
    auto edgeRange = graph.edges(src, galois::MethodFlag::UNPROTECTED);
    for (auto e : edgeRange) {
      GNode dst   = graph.getEdgeDst(e);
      const auto newDist = srcD + graph.getEdgeData(e);

      // Filter out paths that are already longer than
      // the destination node's distance.
      // They won't ever contribute to lowering the destNode's distance.
      if (destDist != UINT32_MAX && newDist > destDist) continue;
#ifdef PERF
      uint32_t oldDist = getPrioData(&prios[dst]);
#else
      uint32_t oldDist = prios[dst].load(std::memory_order_relaxed);
#endif
      // Attempt to CAS the neighbor to a lower distance
      if (changeMin(prios, dst, oldDist, newDist)) {
        fraser_insert(wl, newDist, dst);
      }
    }
  }
  stats->iter = iter;
  stats->emptyWork = emptyWork;
}

template<typename PQ_Type, typename descriptor>
void spawnTasksSTM(PQ_Type& wl, Graph& graph, const GNode& source, int threadNum, std::atomic<uint32_t> *prios, termination_detector_2& detector) {
  // init with source
  auto *me = new descriptor();
  wl.init_thread(me, 0);
  me->op_begin();
  wl.insert(me, 0, source);
  me->op_end();

  stat_t stats[threadNum];
  auto begin = std::chrono::high_resolution_clock::now();

  std::vector<std::thread*> workers;
  cpu_set_t cpuset;
  for (int i = 1; i < threadNum; i++) {
    CPU_ZERO(&cpuset);
    uint64_t coreID = pinning[i];
    CPU_SET(coreID, &cpuset);
    std::thread *newThread;

    if (strict) {
      newThread = new std::thread(
          ThreadTaskSTM_Strict<PQ_Type, descriptor>, std::ref(graph), 
          std::ref(wl), &stats[i], std::ref(prios), std::ref(detector), i);
    } else if (order_batch) {
      newThread = new std::thread(
          ThreadTaskSTMBatch_1_ordered<PQ_Type, descriptor>, std::ref(graph), 
          std::ref(wl), &stats[i], std::ref(prios), std::ref(detector), i);
    } else if (batch) {
      newThread = new std::thread(
          ThreadTaskSTMBatch_1<PQ_Type, descriptor>, std::ref(graph), 
          std::ref(wl), &stats[i], std::ref(prios), std::ref(detector), i);
    } else {
      newThread = new std::thread(
          ThreadTaskSTM<PQ_Type, descriptor>, std::ref(graph), 
          std::ref(wl), &stats[i], std::ref(prios), std::ref(detector), i);
    }
    
    int rc = pthread_setaffinity_np(newThread->native_handle(),
                                sizeof(cpu_set_t), &cpuset);
    if (rc != 0) {
        std::cerr << "Error calling pthread_setaffinity_np: " << rc << "\n";
    }
    workers.push_back(newThread);
  }
  CPU_ZERO(&cpuset);
  CPU_SET(0, &cpuset);
  sched_setaffinity(0, sizeof(cpuset), &cpuset);

  if (strict) ThreadTaskSTM_Strict<PQ_Type, descriptor>(std::ref(graph), std::ref(wl), &stats[0], std::ref(prios), std::ref(detector), 0);
  else if (order_batch) ThreadTaskSTMBatch_1_ordered<PQ_Type, descriptor>(graph, wl, &stats[0], prios, detector, 0);
  else if (batch) ThreadTaskSTMBatch_1<PQ_Type, descriptor>(graph, wl, &stats[0], prios, detector, 0);
  else ThreadTaskSTM<PQ_Type, descriptor>(graph, wl, &stats[0], prios, detector, 0);

  for (std::thread*& worker : workers) {
    worker->join();
    delete worker;
  }

  auto end = std::chrono::high_resolution_clock::now();
  auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(end-begin).count();
  //wl.stat();
  std::cout << "runtime_ms " << ms << "\n";

  if (!wl.empty(me)) {
    std::cout << "not empty!\n";
  }

  for (uint64_t i = 0; i < graph.size(); i++) {
    uint64_t s = prios[i].load(std::memory_order_relaxed);
    if (s == UINT32_MAX) continue;;
    auto& ddata = graph.getData(i);
    ddata = s;
  }

  uint64_t totalIter = 0;
  uint64_t totalEmptyWork = 0;
  for (int i = 0; i < threadNum; i++) {
    totalIter += stats[i].iter;
    totalEmptyWork += stats[i].emptyWork;
  }

  std::cout << "totalEmptyWork " << totalEmptyWork << "\n";

  galois::runtime::reportStat_Single("SSSP-MBQ", "Iterations", totalIter);
  galois::runtime::reportStat_Single("SSSP-MBQ", "Emptywork", totalEmptyWork);
}

template<typename MQ_Type>
void spawnTasks(MQ_Type& wl, Graph& graph, const GNode& source, int threadNum, std::atomic<uint32_t> *prios, termination_detector_2& detector) {
// init with source
  wl.push(0, source);

  stat_t stats[threadNum];
  auto begin = std::chrono::high_resolution_clock::now();

  std::vector<std::thread*> workers;
  cpu_set_t cpuset;
  for (int i = 1; i < threadNum; i++) {
    CPU_ZERO(&cpuset);
    uint64_t coreID = pinning[i];
    CPU_SET(coreID, &cpuset);
    std::thread *newThread = new std::thread(
      MQThreadTask<MQ_Type>, i, std::ref(graph), 
      std::ref(wl), &stats[i], std::ref(prios), std::ref(detector));
    int rc = pthread_setaffinity_np(newThread->native_handle(),
                                sizeof(cpu_set_t), &cpuset);
    if (rc != 0) {
        std::cerr << "Error calling pthread_setaffinity_np: " << rc << "\n";
    }
    workers.push_back(newThread);
  }
  CPU_ZERO(&cpuset);
  CPU_SET(0, &cpuset);
  sched_setaffinity(0, sizeof(cpuset), &cpuset);
  MQThreadTask<MQ_Type>(0, graph, wl, &stats[0], prios, std::ref(detector));

  for (std::thread*& worker : workers) {
    worker->join();
    delete worker;
  }

  auto end = std::chrono::high_resolution_clock::now();
  auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(end-begin).count();
  wl.stat();
  std::cout << "runtime_ms " << ms << "\n";

  if (!wl.empty()) {
    std::cout << "not empty!\n";
  }

  for (uint64_t i = 0; i < graph.size(); i++) {
    uint64_t s = prios[i].load(std::memory_order_relaxed);
    if (s == UINT32_MAX) continue;;
    auto& ddata = graph.getData(i);
    ddata = s;
  }

  uint64_t totalIter = 0;
  uint64_t totalEmptyWork = 0;
  for (int i = 0; i < threadNum; i++) {
    totalIter += stats[i].iter;
    totalEmptyWork += stats[i].emptyWork;
  }

  std::cout << "totalEmptyWork " << totalEmptyWork << "\n";

  galois::runtime::reportStat_Single("SSSP-MBQ", "Iterations", totalIter);
  galois::runtime::reportStat_Single("SSSP-MBQ", "Emptywork", totalEmptyWork);
}

template<typename MQ_Type>
void spawnTasksPIPQ(MQ_Type& wl, Graph& graph, const GNode& source, int threadNum, std::atomic<uint32_t> *prios, termination_detector_2& detector) {
// init with source
  wl.init_thread(0);
  wl.push(0, source);

  stat_t stats[threadNum];
  auto begin = std::chrono::high_resolution_clock::now();

  std::vector<std::thread*> workers;
  cpu_set_t cpuset;
  for (int i = 1; i < threadNum; i++) {
    CPU_ZERO(&cpuset);
    uint64_t coreID = pinning[i];
    CPU_SET(coreID, &cpuset);
    std::thread *newThread = new std::thread(
      PIPQThreadTask<MQ_Type>, i, std::ref(graph), 
      std::ref(wl), &stats[i], std::ref(prios), std::ref(detector));
    int rc = pthread_setaffinity_np(newThread->native_handle(),
                                sizeof(cpu_set_t), &cpuset);
    if (rc != 0) {
        std::cerr << "Error calling pthread_setaffinity_np: " << rc << "\n";
    }
    workers.push_back(newThread);
  }
  CPU_ZERO(&cpuset);
  CPU_SET(0, &cpuset);
  sched_setaffinity(0, sizeof(cpuset), &cpuset);
  PIPQThreadTask<MQ_Type>(0, graph, wl, &stats[0], prios, std::ref(detector));

  for (std::thread*& worker : workers) {
    worker->join();
    delete worker;
  }

  auto end = std::chrono::high_resolution_clock::now();
  auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(end-begin).count();
  //wl.stat();
  std::cout << "runtime_ms " << ms << "\n";

  // if (!wl.empty()) {
  //   std::cout << "not empty!\n";
  // }

  for (uint64_t i = 0; i < graph.size(); i++) {
    uint64_t s = prios[i].load(std::memory_order_relaxed);
    if (s == UINT32_MAX) continue;;
    auto& ddata = graph.getData(i);
    ddata = s;
  }

  uint64_t totalIter = 0;
  uint64_t totalEmptyWork = 0;
  for (int i = 0; i < threadNum; i++) {
    totalIter += stats[i].iter;
    totalEmptyWork += stats[i].emptyWork;
  }

  std::cout << "totalEmptyWork " << totalEmptyWork << "\n";

  galois::runtime::reportStat_Single("SSSP-MBQ", "Iterations", totalIter);
  galois::runtime::reportStat_Single("SSSP-MBQ", "Emptywork", totalEmptyWork);
}

template<typename MQ_Type>
void spawnTasksSMQ(MQ_Type& wl, Graph& graph, const GNode& source, int threadNum, std::atomic<uint32_t> *prios, termination_detector_2& detector) {
// init with source
  wl.init_thread(0);
  wl.insert(0, source);

  stat_t stats[threadNum];
  auto begin = std::chrono::high_resolution_clock::now();

  std::vector<std::thread*> workers;
  cpu_set_t cpuset;
  for (int i = 1; i < threadNum; i++) {
    CPU_ZERO(&cpuset);
    uint64_t coreID = pinning[i];
    CPU_SET(coreID, &cpuset);
    std::thread *newThread = new std::thread(
      SMQThreadTask<MQ_Type>, i, std::ref(graph), 
      std::ref(wl), &stats[i], std::ref(prios), std::ref(detector));
    int rc = pthread_setaffinity_np(newThread->native_handle(),
                                sizeof(cpu_set_t), &cpuset);
    if (rc != 0) {
        std::cerr << "Error calling pthread_setaffinity_np: " << rc << "\n";
    }
    workers.push_back(newThread);
  }
  CPU_ZERO(&cpuset);
  CPU_SET(0, &cpuset);
  sched_setaffinity(0, sizeof(cpuset), &cpuset);
  SMQThreadTask<MQ_Type>(0, graph, wl, &stats[0], prios, std::ref(detector));

  for (std::thread*& worker : workers) {
    worker->join();
    delete worker;
  }

  auto end = std::chrono::high_resolution_clock::now();
  auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(end-begin).count();
  //wl.stat();
  std::cout << "runtime_ms " << ms << "\n";

  // if (!wl.empty()) {
  //   std::cout << "not empty!\n";
  // }

  for (uint64_t i = 0; i < graph.size(); i++) {
    uint64_t s = prios[i].load(std::memory_order_relaxed);
    if (s == UINT32_MAX) continue;;
    auto& ddata = graph.getData(i);
    ddata = s;
  }

  uint64_t totalIter = 0;
  uint64_t totalEmptyWork = 0;
  for (int i = 0; i < threadNum; i++) {
    totalIter += stats[i].iter;
    totalEmptyWork += stats[i].emptyWork;
  }

  std::cout << "totalEmptyWork " << totalEmptyWork << "\n";

  galois::runtime::reportStat_Single("SSSP-MBQ", "Iterations", totalIter);
  galois::runtime::reportStat_Single("SSSP-MBQ", "Emptywork", totalEmptyWork);
}

template<typename MQ_Type>
void spawnTasksLinden(MQ_Type& wl, Graph& graph, const GNode& source, int threadNum, std::atomic<uint32_t> *prios, termination_detector_2& detector) {
// init with source
  insert(wl, 0, source);

  stat_t stats[threadNum];
  auto begin = std::chrono::high_resolution_clock::now();

  std::vector<std::thread*> workers;
  cpu_set_t cpuset;
  for (int i = 1; i < threadNum; i++) {
    CPU_ZERO(&cpuset);
    uint64_t coreID = pinning[i];
    CPU_SET(coreID, &cpuset);
    std::thread *newThread = new std::thread(
      ThreadTaskLinden<MQ_Type>, i, std::ref(graph), 
      std::ref(wl), &stats[i], std::ref(prios), std::ref(detector));
    int rc = pthread_setaffinity_np(newThread->native_handle(),
                                sizeof(cpu_set_t), &cpuset);
    if (rc != 0) {
        std::cerr << "Error calling pthread_setaffinity_np: " << rc << "\n";
    }
    workers.push_back(newThread);
  }
  CPU_ZERO(&cpuset);
  CPU_SET(0, &cpuset);
  sched_setaffinity(0, sizeof(cpuset), &cpuset);
  ThreadTaskLinden<MQ_Type>(0, graph, wl, &stats[0], prios, std::ref(detector));

  for (std::thread*& worker : workers) {
    worker->join();
    delete worker;
  }

  auto end = std::chrono::high_resolution_clock::now();
  auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(end-begin).count();
  //wl.stat();
  std::cout << "runtime_ms " << ms << "\n";

  // if (!wl.empty()) {
  //   std::cout << "not empty!\n";
  // }

  for (uint64_t i = 0; i < graph.size(); i++) {
    uint64_t s = prios[i].load(std::memory_order_relaxed);
    if (s == UINT32_MAX) continue;;
    auto& ddata = graph.getData(i);
    ddata = s;
  }

  uint64_t totalIter = 0;
  uint64_t totalEmptyWork = 0;
  for (int i = 0; i < threadNum; i++) {
    totalIter += stats[i].iter;
    totalEmptyWork += stats[i].emptyWork;
  }

  std::cout << "totalEmptyWork " << totalEmptyWork << "\n";

  galois::runtime::reportStat_Single("SSSP-MBQ", "Iterations", totalIter);
  galois::runtime::reportStat_Single("SSSP-MBQ", "Emptywork", totalEmptyWork);
}

template<typename MQ_Type>
void spawnTasksSpray(MQ_Type& wl, Graph& graph, const GNode& source, int threadNum, std::atomic<uint32_t> *prios, termination_detector_2& detector, thread_data_t *data) {
// init with source
  fraser_insert(wl, 0, source);

  stat_t stats[threadNum];
  auto begin = std::chrono::high_resolution_clock::now();

  std::vector<std::thread*> workers;
  cpu_set_t cpuset;
  for (int i = 1; i < threadNum; i++) {
    CPU_ZERO(&cpuset);
    uint64_t coreID = pinning[i];
    CPU_SET(coreID, &cpuset);
    std::thread *newThread = new std::thread(
      ThreadTaskSpray<MQ_Type>, i, std::ref(graph), 
      std::ref(wl), &stats[i], std::ref(prios), std::ref(detector), data);
    int rc = pthread_setaffinity_np(newThread->native_handle(),
                                sizeof(cpu_set_t), &cpuset);
    if (rc != 0) {
        std::cerr << "Error calling pthread_setaffinity_np: " << rc << "\n";
    }
    workers.push_back(newThread);
  }
  CPU_ZERO(&cpuset);
  CPU_SET(0, &cpuset);
  sched_setaffinity(0, sizeof(cpuset), &cpuset);
  ThreadTaskSpray<MQ_Type>(0, graph, wl, &stats[0], prios, std::ref(detector), data);

  for (std::thread*& worker : workers) {
    worker->join();
    delete worker;
  }

  auto end = std::chrono::high_resolution_clock::now();
  auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(end-begin).count();
  //wl.stat();
  std::cout << "runtime_ms " << ms << "\n";

  // if (!wl.empty()) {
  //   std::cout << "not empty!\n";
  // }

  for (uint64_t i = 0; i < graph.size(); i++) {
    uint64_t s = prios[i].load(std::memory_order_relaxed);
    if (s == UINT32_MAX) continue;;
    auto& ddata = graph.getData(i);
    ddata = s;
  }

  uint64_t totalIter = 0;
  uint64_t totalEmptyWork = 0;
  for (int i = 0; i < threadNum; i++) {
    totalIter += stats[i].iter;
    totalEmptyWork += stats[i].emptyWork;
  }

  std::cout << "totalEmptyWork " << totalEmptyWork << "\n";

  galois::runtime::reportStat_Single("SSSP-MBQ", "Iterations", totalIter);
  galois::runtime::reportStat_Single("SSSP-MBQ", "Emptywork", totalEmptyWork);
}

template<bool usePrefetch=true>
uint32_t MQAlgo(Graph& graph, const GNode& source, int threadNum, int queueNum) {
  std::cout << "threads = " << threadNum << "\n";
  std::cout << "queues = " << queueNum << "\n";
  std::cout << "batchSizePop = " << batch1 << "\n";
  std::cout << "batchSizePush = " << batch2 << "\n";
  std::cout << "stickiness = " << stickiness << "\n";
  std::cout << "prefetch " << usePrefetch << "\n";

  termination_detector_2 detector(threadNum);

  // The distance array that records the latest distances
  std::atomic<uint32_t> *prios = new std::atomic<uint32_t>[graph.size()];
  for (uint i = 0; i < graph.size(); i++) {
    prios[i].store(UINT32_MAX, std::memory_order_relaxed);
  }
  prios[source] = 0;
  graph.getData(source) = 0;

  // Prefetcher lambda for reducing cache misses on load to a 
  // vertex's distance
  std::function<void(uint32_t)> prefetcher = [&] (uint32_t v) -> void {
    __builtin_prefetch(&prios[v], 0, 3);

    // the first and last of edges
    graph.prefetchEdgeStart(v);
    graph.prefetchEdgeEnd(v);
  };

  if (algo == MQ) {
    using MQ_Type = mbq::MultiQueue<decltype(prefetcher), std::greater<PQElement>, uint32_t, uint32_t, usePrefetch>;
    MQ_Type wl(prefetcher, queueNum, threadNum, batch1, batch2, stickiness);
    spawnTasks<MQ_Type>(wl, graph, source, threadNum, prios, detector);

  } else if (algo == MQBucket) {
    // MQBucket
    std::cout << "buckets = " << bucketNum << "\n";
    std::cout << "delta = " << stepShift << "\n";

    // Lambda for mapping a priority to a priority level
    std::function<mbq::BucketID(uint32_t)> getBucketID = [&] (uint32_t v) -> mbq::BucketID {
      uint32_t d = prios[v].load(std::memory_order_acquire);
      return ((mbq::BucketID)d >> stepShift);
    };

    using MQ_Bucket_Type = mbq::MultiBucketQueue<decltype(getBucketID), decltype(prefetcher), std::greater<uint32_t>, uint32_t, uint32_t, usePrefetch>;
    MQ_Bucket_Type wl(getBucketID, prefetcher, queueNum, threadNum, stepShift, bucketNum, batch1, batch2, mbq::increasing, stickiness);
    spawnTasks<MQ_Bucket_Type>(wl, graph, source, threadNum, prios, detector);
  } else if (algo == SkipHashPQ) {
    // SkipHashPQ

    #define _ALG eager_noext_c1_t
    #define _OREC orec_po_t
    #define _CLOCK rdtscp_clock_t

    using descriptor = _ALG<_OREC, clock_policy::_CLOCK>;
    using map = skiphash_pq_relaxed<uint32_t, uint32_t, _ALG<_OREC, clock_policy::_CLOCK>>;
    auto *me = new descriptor();

    config_t cfg;
    cfg.max_levels = sl_max_levels;
    cfg.buckets = buckets;
    cfg.chunksize = chunksize;
    cfg.num_queues = num_chunks;

    std::cout << "cfg.max_levels: " << static_cast<int>(cfg.max_levels) << "\n";
    std::cout << "cfg.buckets: " << cfg.buckets << "\n";
    std::cout << "cfg.chunksize: " << cfg.chunksize << "\n";
    std::cout << "cfg.num_queues: " << cfg.num_queues << "\n";

    map pq(me, &cfg);
    spawnTasksSTM<map, descriptor>(pq, graph, source, threadNum, prios, detector);
  } else if (algo == PIPQ) {
    int heap_list_size = 10000000;
    int cntr_tsh = 10;
    int cntr_max = 100;

    using pipq_t = pq_ns::pipq<uint32_t>;
    auto numa_pq_ds = pipq_t(heap_list_size, threadNum, cntr_tsh, cntr_max, pinning);
    numa_pq_ds.PQInit();
    spawnTasksPIPQ<pipq_t>(numa_pq_ds, graph, source, threadNum, prios, detector);
  } else if (algo == SMQ) {
    const size_t steal_probability = 16; // 1/8 probability of stealing
    const size_t steal_batch_size = 128; // size of batch to steal

    using smq_t = smq_ns::StealingMultiQueue<std::pair<uint32_t,uint32_t>,uint32_t,steal_probability,steal_batch_size,true>;
    auto smq_ds = smq_t(threadNum);
    spawnTasksSMQ<smq_t>(smq_ds, graph, source, threadNum, prios, detector);
  } else if (algo == Linden) {
    int max_offset = 32;
    _init_gc_subsystem();
    pq_t * linden_pq = pq_init(max_offset);
    spawnTasksLinden(linden_pq, graph, source, threadNum, prios, detector);
  } else if (algo == Spray) {
    *levelmax = 32;
    thread_data_t *data = (thread_data_t *)malloc(threadNum * sizeof(thread_data_t));
    for (int i = 0; i < threadNum; i++) {
      data[i].seed = rand();
      data[i].seed2 = rand();
      data[i].nb_threads = threadNum;
    }
    //ssalloc_init();
    seeds = seed_rand();
    std::cout << "Calling sl_set_new()\n";
    sl_intset_t * sl_spray = sl_set_new(); //sl_add_val(set, 0, src, 0);
    std::cout << "Created set\n";
    spawnTasksSpray(sl_spray, graph, source, threadNum, prios, detector, data);
  } else {
    std::cout << "ERROR: invalid data structure\n";
    std::terminate();
  }
  
  return prios[destNode].load(std::memory_order_relaxed);
}

int main(int argc, char** argv) {
  galois::SharedMemSys G;
  LonestarStart(argc, argv, name, desc, url, &inputFile);

  galois::StatTimer totalTime("TimerTotal");
  totalTime.start();

  Graph graph;
  GNode source;

  std::cout << "Reading from file: " << inputFile << "\n";
  galois::graphs::readGraph(graph, inputFile);
  std::cout << "Read " << graph.size() << " nodes, " << graph.sizeEdges()
            << " edges\n";

  if (startNode >= graph.size() || reportNode >= graph.size()) {
    std::cerr << "failed to set report: " << reportNode
              << " or failed to set source: " << startNode << "\n";
    assert(0);
    abort();
  }

  auto it = graph.begin();
  std::advance(it, startNode.getValue());
  source = *it;
  it     = graph.begin();

  size_t approxNodeData = graph.size() * 64;
  galois::preAlloc(numThreads +
                   approxNodeData / galois::runtime::pagePoolSize());
  galois::reportPageAlloc("MeminfoPre");

  galois::do_all(galois::iterate(graph),
                 [&graph](GNode n) { graph.getData(n) = SSSP::DIST_INFINITY; });

  graph.getData(source) = 0;

  std::cout << "Running " << ALGO_NAMES[algo] << " algorithm\n";

  galois::StatTimer autoAlgoTimer("AutoAlgo_0");
  galois::StatTimer execTime("Timer_0");
  execTime.start();
  uint32_t destDist = SSSP::DIST_INFINITY;

  namespace gwl = galois::worklists;
  using PSchunk4 = gwl::PerSocketChunkFIFO<4>;
  using PSchunk8 = gwl::PerSocketChunkFIFO<8>;
  using PSchunk16 = gwl::PerSocketChunkFIFO<16>;
  using PSchunk32 = gwl::PerSocketChunkFIFO<32>;
  using PSchunk64 = gwl::PerSocketChunkFIFO<64>;
  using PSchunk128 = gwl::PerSocketChunkFIFO<128>;
  using PSchunk256 = gwl::PerSocketChunkFIFO<256>;
  using OBIM4 = gwl::OrderedByIntegerMetric<UpdateRequestIndexer, PSchunk4>;
  using OBIM8 = gwl::OrderedByIntegerMetric<UpdateRequestIndexer, PSchunk8>;
  using OBIM16 = gwl::OrderedByIntegerMetric<UpdateRequestIndexer, PSchunk16>;
  using OBIM32 = gwl::OrderedByIntegerMetric<UpdateRequestIndexer, PSchunk32>;
  using OBIM64 = gwl::OrderedByIntegerMetric<UpdateRequestIndexer, PSchunk64>;
  using OBIM128 = gwl::OrderedByIntegerMetric<UpdateRequestIndexer, PSchunk128>;
  using OBIM256 = gwl::OrderedByIntegerMetric<UpdateRequestIndexer, PSchunk256>;
  using PMOD4 = gwl::AdaptiveOrderedByIntegerMetric<UpdateRequestIndexer, PSchunk4>;
  using PMOD8 = gwl::AdaptiveOrderedByIntegerMetric<UpdateRequestIndexer, PSchunk8>;
  using PMOD16 = gwl::AdaptiveOrderedByIntegerMetric<UpdateRequestIndexer, PSchunk16>;
  using PMOD32 = gwl::AdaptiveOrderedByIntegerMetric<UpdateRequestIndexer, PSchunk32>;
  using PMOD64 = gwl::AdaptiveOrderedByIntegerMetric<UpdateRequestIndexer, PSchunk64>;
  using PMOD128 = gwl::AdaptiveOrderedByIntegerMetric<UpdateRequestIndexer, PSchunk128>;
  using PMOD256 = gwl::AdaptiveOrderedByIntegerMetric<UpdateRequestIndexer, PSchunk256>;


  switch (algo) {
  case deltaStepOBIM:
    std::cout << "running OBIM with chunk size " << chunk << "\n";
    switch (chunk) {
      case 4:
        destDist = deltaStepAlgo<UpdateRequest, OBIM4>(
                    graph, source, ReqPushWrap(), OutEdgeRangeFn{graph});
        break;
      case 8:
        destDist = deltaStepAlgo<UpdateRequest, OBIM8>(
                    graph, source, ReqPushWrap(), OutEdgeRangeFn{graph});
        break;
      case 16:
        destDist = deltaStepAlgo<UpdateRequest, OBIM16>(
                    graph, source, ReqPushWrap(), OutEdgeRangeFn{graph});
        break;
      case 32:
        destDist = deltaStepAlgo<UpdateRequest, OBIM32>(
                    graph, source, ReqPushWrap(), OutEdgeRangeFn{graph});
        break;
      case 64:
        destDist = deltaStepAlgo<UpdateRequest, OBIM64>(
                    graph, source, ReqPushWrap(), OutEdgeRangeFn{graph});
        break;
      case 128:
        destDist = deltaStepAlgo<UpdateRequest, OBIM128>(
                    graph, source, ReqPushWrap(), OutEdgeRangeFn{graph});
        break;
      case 256:
        destDist = deltaStepAlgo<UpdateRequest, OBIM256>(
                    graph, source, ReqPushWrap(), OutEdgeRangeFn{graph});
        break;
      default:
        std::cerr << "ERROR: unkown chunk size\n";
    }
    break;
  case deltaStepPMOD:
    std::cout << "running PMOD with chunk size " << chunk << "\n";
    switch (chunk) {
      case 4:
        destDist = deltaStepAlgo<UpdateRequest, PMOD4>(
                    graph, source, ReqPushWrap(), OutEdgeRangeFn{graph});
        break;
      case 8:
        destDist = deltaStepAlgo<UpdateRequest, PMOD8>(
                    graph, source, ReqPushWrap(), OutEdgeRangeFn{graph});
        break;
      case 16:
        destDist = deltaStepAlgo<UpdateRequest, PMOD16>(
                    graph, source, ReqPushWrap(), OutEdgeRangeFn{graph});
        break;
      case 32:
        destDist = deltaStepAlgo<UpdateRequest, PMOD32>(
                    graph, source, ReqPushWrap(), OutEdgeRangeFn{graph});
        break;
      case 64:
        destDist = deltaStepAlgo<UpdateRequest, PMOD64>(
                    graph, source, ReqPushWrap(), OutEdgeRangeFn{graph});
        break;
      case 128:
        destDist = deltaStepAlgo<UpdateRequest, PMOD128>(
                    graph, source, ReqPushWrap(), OutEdgeRangeFn{graph});
        break;
      case 256:
        destDist = deltaStepAlgo<UpdateRequest, PMOD256>(
                    graph, source, ReqPushWrap(), OutEdgeRangeFn{graph});
        break;
      default:
        std::cerr << "ERROR: unkown chunk size\n";
    }
    break;
  case MQ:
    std::cout << "running MQ\n";
    if (prefetch == 1) destDist = MQAlgo<true>(graph, source, threadNum, queueNum);
    else  destDist = MQAlgo<false>(graph, source, threadNum, queueNum);
    break;
  case MQBucket:
    std::cout << "running MQBucket\n";
    if (prefetch == 1) destDist = MQAlgo<true>(graph, source, threadNum, queueNum); 
    else destDist = MQAlgo<false>(graph, source, threadNum, queueNum); 
    break; 
  case SkipHashPQ:
    std::cout << "running SkipHashPQ\n";
    // SkipHashPQ
    destDist = MQAlgo<false>(graph, source, threadNum, queueNum); 
    break;
  case SMQ:
  case Linden:
  case Spray:
  case PIPQ:
    std::cout << "running " << ALGO_NAMES[algo] << "\n";
    destDist = MQAlgo<false>(graph, source, threadNum, queueNum);
    break;
  default:
    std::abort();
  }

  execTime.stop();
  totalTime.stop();
  std::cout << "destination node distance = " << destDist << "\n";

  return 0;
}
