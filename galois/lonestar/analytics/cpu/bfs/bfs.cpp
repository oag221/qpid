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

// #define SPDLOG_COMPILED_LIB  // Prevents it from looking for pre-compiled binaries
// 
//#include <spdlog/fmt/bundled/format.h> // Force use of internal fmt

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

// #include <spdlog/spdlog.h>
// #include "spdlog/async.h"
// #include "spdlog/sinks/basic_file_sink.h"

#include "llvm/Support/CommandLine.h"

#include <iostream>
#include <deque>
#include <type_traits>

#include <include/MultiBucketQueue.h>
#include <include/MultiQueue.h>

#include <include-skiphashpq/optstm2/eager_noext_c1.h>
#include <include-skiphashpq/skiphash_pq_relaxed.h>
#include "../config.h"

#include <include-pipq/pipq_impl.h>

#include <include-smq/smq_impl.h>

#include <include-linden/linden.h>
#include "gc/ptst.h"

#include "intset.h"
#include <common/include/random.h>

#include "../termination_helper.h"

OPTSTM2_GLOBALS_INITIALIZER;

const int pinning[96] = {0,2,4,6,8,10,12,14,16,18,20,22,24,26,28,30,32,34,36,38,40,42,44,46,1,3,5,7,9,11,13,15,17,19,21,23,25,27,29,31,33,35,37,39,41,43,45,47,48,50,52,54,56,58,60,62,64,66,68,70,72,74,76,78,80,82,84,86,88,90,92,94,49,51,53,55,57,59,61,63,65,67,69,71,73,75,77,79,81,83,85,87,89,91,93,95};

alignas(64) extern uint8_t levelmax[64];
__thread unsigned long *seeds;


// #define PERF 1

namespace cll = llvm::cl;

static const char* name = "Breadth-first Search";

static const char* desc =
    "Computes the shortest path from a source node to all nodes in a directed "
    "graph using a modified Bellman-Ford algorithm";

static const char* url = "breadth_first_search";

static cll::opt<std::string>
    inputFile(cll::Positional, cll::desc("<input file>"), cll::Required);
static cll::opt<unsigned int>
    startNode("startNode",
              cll::desc("Node to start search from (default value 0)"),
              cll::init(0));
static cll::opt<unsigned int>
    reportNode("reportNode",
               cll::desc("Node to report distance to (default value 1)"),
               cll::init(1));
static cll::opt<unsigned int>
    chunk("chunk",
              cll::desc("chunk size for tuning"),
              cll::init(64));
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
              cll::desc("number of buckets in a bucket queue"),
              cll::init(64));
static cll::opt<unsigned int>
    batch1("batch1",
              cll::desc("batch size for popping"),
              cll::init(1));
static cll::opt<unsigned int>
    batch2("batch2",
              cll::desc("batch size for pushing"),
              cll::init(1));
static cll::opt<unsigned int>
    prefetch("prefetch",
              cll::desc("prefetching"),
              cll::init(0));
static cll::opt<unsigned int>
    stickiness("stick",
              cll::desc("stickiness"),
              cll::init(1));
static cll::opt<unsigned int>
    stepShift("delta",
        cll::desc("Shift value for the deltastep"),
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
        cll::desc("Whether or not SkipHashPQ should use a batch for inserting"),
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

// static cll::opt<unsigned int>
//     strict("strict",
//         cll::desc("Whether or not to use strict"),
//         cll::init(0));



enum Exec { SERIAL, PARALLEL };

enum Algo { OBIM, PMOD, MQ, MQBucket, SkipHashPQ, PIPQ, SMQ, Linden, Spray };

const char* const ALGO_NAMES[] = {"OBIM", "PMOD", "MQ", "MQBucket", "SkipHashPQ", "PIPQ", "SMQ", "Linden", "Spray"};

static cll::opt<Exec> execution(
    "exec",
    cll::desc("Choose SERIAL or PARALLEL execution (default value PARALLEL):"),
    cll::values(clEnumVal(SERIAL, "SERIAL"), clEnumVal(PARALLEL, "PARALLEL")),
    cll::init(PARALLEL));

static cll::opt<Algo> algo(
    "algo", cll::desc("Choose an algorithm (default value OBIM):"),
    cll::values(clEnumVal(OBIM, "OBIM"), clEnumVal(PMOD, "PMOD"),
                clEnumVal(MQ, "MQ"), clEnumVal(MQBucket, "MQBucket"),
                clEnumVal(SkipHashPQ, "SkipHashPQ"), clEnumVal(PIPQ, "PIPQ"),
                clEnumVal(SMQ, "SMQ"), clEnumVal(Linden, "Linden"), clEnumVal(Spray, "Spray")),
    cll::init(OBIM));

using Graph =
    galois::graphs::LC_CSR_Graph<std::atomic<uint32_t>, void>::with_no_lockable<true>::type;
//::with_numa_alloc<true>::type;

using GNode = Graph::GraphNode;

constexpr static const bool TRACK_WORK          = false;
constexpr static const unsigned CHUNK_SIZE      = 256U;
constexpr static const ptrdiff_t EDGE_TILE_SIZE = 256;

using BFS = BFS_SSSP<Graph, uint32_t, false, EDGE_TILE_SIZE>;

using UpdateRequest       = BFS::UpdateRequest;
using UpdateRequestIndexer= BFS::UpdateRequestIndexer;
using Dist                = BFS::Dist;
using SrcEdgeTile         = BFS::SrcEdgeTile;
using SrcEdgeTileMaker    = BFS::SrcEdgeTileMaker;
using SrcEdgeTilePushWrap = BFS::SrcEdgeTilePushWrap;
using ReqPushWrap         = BFS::ReqPushWrap;
using OutEdgeRangeFn      = BFS::OutEdgeRangeFn;
using TileRangeFn         = BFS::TileRangeFn;


struct main_globals_t {
  atomic_int running;
  bool start;
};

main_globals_t glob = {0,};
pthread_barrier_t WaitForAll;

template <bool CONCURRENT, typename T, typename OBIMTy, typename P, typename R>
void OBIMAlgo(Graph& graph, GNode source, const P& pushWrap,
               const R& edgeRange) {

  galois::GAccumulator<size_t> WLEmptyWork;

  graph.getData(source) = 0;
  galois::InsertBag<T> initBag;
  pushWrap(initBag, source, 0, "parallel");

  auto begin = std::chrono::high_resolution_clock::now();

  galois::for_each(
      galois::iterate(initBag),
      [&](const T& item, auto& ctx) {
        constexpr galois::MethodFlag flag = galois::MethodFlag::UNPROTECTED;
        const auto& sdata                 = graph.getData(item.src, flag);

        if (sdata < item.dist) {
          WLEmptyWork += 1;
          return;
        }
        const Dist newDist = sdata + 1;

        for (auto ii : edgeRange(item)) {
          GNode dst     = graph.getEdgeDst(ii);
          auto& ddist   = graph.getData(dst, flag);
          Dist oldDist  = galois::atomicMin<uint32_t>(ddist, newDist);
          if (newDist < oldDist) {
            pushWrap(ctx, dst, newDist);
          }
        }
      },
      galois::wl<OBIMTy>(UpdateRequestIndexer{stepShift}), galois::loopname("runBFS"),
      galois::disable_conflict_detection());

  auto end = std::chrono::high_resolution_clock::now();
  auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(end-begin).count();
  std::cout << "runtime_ms " << ms << "\n";

  galois::runtime::reportStat_Single("runBFS", "EmptyWork",
                                      WLEmptyWork.reduce());
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

template<typename MQ>
void MQThreadTask(Graph& graph, MQ &wl, stat_t *stats, std::atomic<uint32_t> *prios, termination_detector_2& detector) {
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
    //auto item = wl.pop();
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

    // Iterate neighbors and see if their distances can be lowered
    uint32_t newDist = srcD + 1;
    auto edgeRange = graph.edges(src, galois::MethodFlag::UNPROTECTED);
    for (auto e : edgeRange) {
      GNode dst   = graph.getEdgeDst(e);
#ifdef PERF
      uint32_t d = getPrioData(&prios[dst]);
#else
      uint32_t d = prios[dst].load(std::memory_order_relaxed);
#endif
      // Attempt to CAS the neighbor to a lower distance
      if (changeMin(prios, dst, d, newDist)) {
        wl.push(newDist, dst);
      }
    }
    wl.pushInternal();
  }
  stats->iter = iter;
  stats->emptyWork = emptyWork;
}

template<typename MQ>
void MQThreadTaskPIPQ(int tid, Graph& graph, MQ &wl, stat_t *stats, std::atomic<uint32_t> *prios, termination_detector_2& detector) {
  uint64_t iter = 0UL;
  uint64_t emptyWork = 0UL;
  uint dist;
  GNode src;
  using extract_ret_t = std::optional<std::pair<uint,GNode>>;
  auto call_extract = [&]() {
    return wl.extract_min();
  };
  
  if (tid) { // tid == 0 already init to insert the source
    wl.init_thread(tid);
  }

  while (true) {
    //auto item = wl.pop();
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

    // Iterate neighbors and see if their distances can be lowered
    uint32_t newDist = srcD + 1;
    auto edgeRange = graph.edges(src, galois::MethodFlag::UNPROTECTED);
    for (auto e : edgeRange) {
      GNode dst   = graph.getEdgeDst(e);
#ifdef PERF
      uint32_t d = getPrioData(&prios[dst]);
#else
      uint32_t d = prios[dst].load(std::memory_order_relaxed);
#endif
      // Attempt to CAS the neighbor to a lower distance
      if (changeMin(prios, dst, d, newDist)) {
        wl.push(newDist, dst);
      }
    }
  }
  stats->iter = iter;
  stats->emptyWork = emptyWork;
}

template<typename MQ>
void MQThreadTaskSMQ(int tid, Graph& graph, MQ &wl, stat_t *stats, std::atomic<uint32_t> *prios, termination_detector_2& detector) {
  uint64_t iter = 0UL;
  uint64_t emptyWork = 0UL;
  uint dist;
  GNode src;
  using extract_ret_t = std::optional<std::pair<uint,GNode>>;
  auto call_extract = [&]() {
    return wl.extract_min();
  };
  
  if (tid) { // tid == 0 already init to insert the source
    wl.init_thread(tid);
  }

  while (true) {
    //auto item = wl.pop();
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

    // Iterate neighbors and see if their distances can be lowered
    uint32_t newDist = srcD + 1;
    auto edgeRange = graph.edges(src, galois::MethodFlag::UNPROTECTED);
    for (auto e : edgeRange) {
      GNode dst   = graph.getEdgeDst(e);
#ifdef PERF
      uint32_t d = getPrioData(&prios[dst]);
#else
      uint32_t d = prios[dst].load(std::memory_order_relaxed);
#endif
      // Attempt to CAS the neighbor to a lower distance
      if (changeMin(prios, dst, d, newDist)) {
        wl.insert(newDist, dst);
      }
    }
  }
  stats->iter = iter;
  stats->emptyWork = emptyWork;
}

template<typename MQ>
void MQThreadTaskLinden(int tid, Graph& graph, MQ &wl, stat_t *stats, std::atomic<uint32_t> *prios, termination_detector_2& detector) {
  uint64_t iter = 0UL;
  uint64_t emptyWork = 0UL;
  uint dist;
  GNode src;
  using extract_ret_t = std::optional<std::pair<uint,GNode>>;
  auto call_extract = [&]() {
    return extract_min(wl);
  };

  while (true) {
    //auto item = wl.pop();
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

    // Iterate neighbors and see if their distances can be lowered
    uint32_t newDist = srcD + 1;
    auto edgeRange = graph.edges(src, galois::MethodFlag::UNPROTECTED);
    for (auto e : edgeRange) {
      GNode dst   = graph.getEdgeDst(e);
#ifdef PERF
      uint32_t d = getPrioData(&prios[dst]);
#else
      uint32_t d = prios[dst].load(std::memory_order_relaxed);
#endif
      // Attempt to CAS the neighbor to a lower distance
      if (changeMin(prios, dst, d, newDist)) {
        insert(wl, newDist, dst);
      }
    }
  }
  stats->iter = iter;
  stats->emptyWork = emptyWork;
}

template<typename MQ>
void MQThreadTaskSpray(int tid, Graph& graph, MQ &wl, stat_t *stats, std::atomic<uint32_t> *prios, termination_detector_2& detector, thread_data_t *data) {
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
    //auto item = wl.pop();
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

    // Iterate neighbors and see if their distances can be lowered
    uint32_t newDist = srcD + 1;
    auto edgeRange = graph.edges(src, galois::MethodFlag::UNPROTECTED);
    for (auto e : edgeRange) {
      GNode dst   = graph.getEdgeDst(e);
#ifdef PERF
      uint32_t d = getPrioData(&prios[dst]);
#else
      uint32_t d = prios[dst].load(std::memory_order_relaxed);
#endif
      // Attempt to CAS the neighbor to a lower distance
      if (changeMin(prios, dst, d, newDist)) {
        fraser_insert(wl, newDist, dst);
      }
    }
  }
  stats->iter = iter;
  stats->emptyWork = emptyWork;
}

template<typename MQ, typename descriptor>
void MQThreadTaskSTM(Graph& graph, MQ &wl, stat_t *stats, std::atomic<uint32_t> *prios, termination_detector_2& detector, int tid) {
  uint64_t iter = 0UL;
  uint64_t emptyWork = 0UL;
  uint dist;
  GNode src;
  
  auto* me = new descriptor();
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

    // Iterate neighbors and see if their distances can be lowered
    uint32_t newDist = srcD + 1;
    auto edgeRange = graph.edges(src, galois::MethodFlag::UNPROTECTED);
    for (auto e : edgeRange) {
      GNode dst   = graph.getEdgeDst(e);
#ifdef PERF
      uint32_t d = getPrioData(&prios[dst]);
#else
      uint32_t d = prios[dst].load(std::memory_order_relaxed);
#endif
      // Attempt to CAS the neighbor to a lower distance
      if (changeMin(prios, dst, d, newDist)) {
        me->op_begin();
        wl.insert(me, newDist, dst);
        me->op_end();
      }
    }
  }
  stats->iter = iter;
  stats->emptyWork = emptyWork;
 // wl.thread_terminate();
}

template<typename MQ, typename descriptor>
void MQThreadTaskSTM_Strict(Graph& graph, MQ &wl, stat_t *stats, std::atomic<uint32_t> *prios, termination_detector_2& detector, int tid) {
  uint64_t iter = 0UL;
  uint64_t emptyWork = 0UL;
  uint dist;
  GNode src;
  auto* me = new descriptor();
  using extract_ret_t = std::optional<std::pair<uint,GNode>>;
  auto call_extract = [&]() {
    me->op_begin();
    auto ret = wl.extract_min_strict(me); 
    me->op_end();
    return ret;
  };
  if (tid) wl.init_thread(me, tid);
  else wl.re_init_thread(me);

  #if defined(PROFILING) //&& defined(PROFILING_SERIAL)
  int cnt = 0;
  int print_rate = 5000;
  std::map<int,int> prio_freqs;
  if (tid > 0) std::cout << "WARNING: PROFILING is defined, but using >1 thread - subsequent call to dump_ht() is serial\n";
  #endif

  while (true) {
    auto ret_term = try_extract<extract_ret_t>(detector, call_extract); // handles termination detection
    if (!ret_term) break; // TERMINATE
    std::tie(dist, src) = ret_term.value();

    #ifdef PROFILING
    if (++cnt % print_rate == 0) {
        std::cout << "(cnt=" << cnt << ")\n";
        wl.dump_ht();
        std::cout << "\n\n";
    }
    #endif

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

    // Iterate neighbors and see if their distances can be lowered
    uint32_t newDist = srcD + 1;
    auto edgeRange = graph.edges(src, galois::MethodFlag::UNPROTECTED);
    for (auto e : edgeRange) {
      GNode dst   = graph.getEdgeDst(e);
#ifdef PERF
      uint32_t d = getPrioData(&prios[dst]);
#else
      uint32_t d = prios[dst].load(std::memory_order_relaxed);
#endif
      // Attempt to CAS the neighbor to a lower distance
      if (changeMin(prios, dst, d, newDist)) {
        me->op_begin();
        wl.insert(me, newDist, dst);
        me->op_end();

        #ifdef PROFILING
        if (prio_freqs.find(newDist) != prio_freqs.end()) prio_freqs[newDist] += 1;
        else prio_freqs[newDist] = 1;
        #endif
      }
    }
  }
  stats->iter = iter;
  stats->emptyWork = emptyWork;
  //wl.thread_terminate();
  #ifdef PROFILING
  int tot_prios = 0;
  for (const auto& [key, value] : prio_freqs) {
      std::cout << "Prio: " << key << ", Freq: " << value << "\n";
      tot_prios++;
  }
  std::cout << "Unique priorities: " << tot_prios << "\n";
  #endif
}

template<typename MQ, typename descriptor, typename OPTSTM>
void MQThreadTaskSTMBatch_1(Graph& graph, MQ &wl, stat_t *stats, std::atomic<uint32_t> *prios, termination_detector_2& detector, int tid) {
  uint64_t iter = 0UL;
  uint64_t emptyWork = 0UL;
  uint dist;
  GNode src;
  using extract_ret_t = std::optional<std::pair<uint,GNode>>;
  auto* me = new descriptor();
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

    // Iterate neighbors and see if their distances can be lowered
    uint32_t newDist = srcD + 1;
    auto edgeRange = graph.edges(src, galois::MethodFlag::UNPROTECTED);

    for (auto e : edgeRange) {
      GNode dst   = graph.getEdgeDst(e);
#ifdef PERF
      uint32_t d = getPrioData(&prios[dst]);
#else
      uint32_t d = prios[dst].load(std::memory_order_relaxed);
#endif
      // Attempt to CAS the neighbor to a lower distance
      
      if (changeMin(prios, dst, d, newDist)) {
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
  //wl.thread_terminate(); //! COMMENT OUT WHEN RUNNING TESTS
  // double avg_inserted = (double)per_loop / num_loops;
  // std::cout << "avg inserted per single removal: " << avg_inserted << "\n";
}

template<typename MQ, typename descriptor, typename OPTSTM>
void MQThreadTaskSTMBatch_1_ordered(Graph& graph, MQ &wl, stat_t *stats, std::atomic<uint32_t> *prios, termination_detector_2& detector, int tid) {
  uint64_t iter = 0UL;
  uint64_t emptyWork = 0UL;
  uint dist;
  GNode src;
  
  using extract_ret_t = std::optional<std::pair<uint,GNode>>;
  auto* me = new descriptor();
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

    // Iterate neighbors and see if their distances can be lowered
    uint32_t newDist = srcD + 1;
    auto edgeRange = graph.edges(src, galois::MethodFlag::UNPROTECTED);

    for (auto e : edgeRange) {
      GNode dst   = graph.getEdgeDst(e);
#ifdef PERF
      uint32_t d = getPrioData(&prios[dst]);
#else
      uint32_t d = prios[dst].load(std::memory_order_relaxed);
#endif
      // Attempt to CAS the neighbor to a lower distance
      
      if (changeMin(prios, dst, d, newDist)) {
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
  //wl.thread_terminate(); //! COMMENT OUT WHEN RUNNING TESTS
  // double avg_inserted = (double)per_loop / num_loops;
  // std::cout << "avg inserted per single removal: " << avg_inserted << "\n";
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
      MQThreadTask<MQ_Type>, std::ref(graph), 
      std::ref(wl), &stats[i], std::ref(prios), std::ref(detector)
    );
    int rc = pthread_setaffinity_np(newThread->native_handle(),
                                    sizeof(cpu_set_t), &cpuset);
    if (rc != 0) {
        std::cerr << "Error calling pthread_setaffinity_np: " << rc << "\n";
    }
    workers.push_back(newThread);
  }
  CPU_ZERO(&cpuset);
  CPU_SET(pinning[0], &cpuset);
  sched_setaffinity(0, sizeof(cpuset), &cpuset);
  MQThreadTask<MQ_Type>(graph, wl, &stats[0], prios, std::ref(detector));
  for (std::thread*& worker : workers) {
    worker->join();
    delete worker;
  }

  auto end = std::chrono::high_resolution_clock::now();
  auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(end-begin).count();
  if (!wl.empty()) {
    std::cout << "not empty\n";
  }

  wl.stat();
  std::cout << "runtime_ms " << ms << "\n";

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
  galois::runtime::reportStat_Single("BFS-MQBucket", "Iterations", totalIter);
  galois::runtime::reportStat_Single("BFS-MQBucket", "EmptyWork", totalEmptyWork);
}

template<typename MQ_Type, typename descriptor, typename OPTSTM>
void spawnTasksSTM(MQ_Type& wl, Graph& graph, const GNode& source, int threadNum, std::atomic<uint32_t> *prios, termination_detector_2& detector_2) {
  // init with source
  auto* me = new descriptor();
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
      // strict
      newThread = new std::thread(
        MQThreadTaskSTM_Strict<MQ_Type, descriptor>, std::ref(graph), 
        std::ref(wl), &stats[i], std::ref(prios), std::ref(detector_2), i);
    } else if (order_batch) {
      // batching, ordered = has to be batch_1 if ordered
      newThread = new std::thread(
          MQThreadTaskSTMBatch_1_ordered<MQ_Type, descriptor, OPTSTM>, std::ref(graph), 
          std::ref(wl), &stats[i], std::ref(prios), std::ref(detector_2), i);
    } else if (batch) { 
      // batch-ins and batch-del
      newThread = new std::thread(
        MQThreadTaskSTMBatch_1<MQ_Type, descriptor, OPTSTM>, std::ref(graph), 
        std::ref(wl), &stats[i], std::ref(prios), std::ref(detector_2), i);
    } else {
      // batch-del, but no batch-ins
      newThread = new std::thread(
          MQThreadTaskSTM<MQ_Type, descriptor>, std::ref(graph), 
          std::ref(wl), &stats[i], std::ref(prios), std::ref(detector_2), i);
    }
    
    int rc = pthread_setaffinity_np(newThread->native_handle(),
                                    sizeof(cpu_set_t), &cpuset);
    if (rc != 0) {
        std::cerr << "Error calling pthread_setaffinity_np: " << rc << "\n";
    }
    workers.push_back(newThread);
  }
  
  CPU_ZERO(&cpuset);
  CPU_SET(pinning[0], &cpuset);
  sched_setaffinity(0, sizeof(cpuset), &cpuset);

  if (strict) {
    MQThreadTaskSTM_Strict<MQ_Type, descriptor>(std::ref(graph), std::ref(wl), &stats[0], std::ref(prios), std::ref(detector_2), 0);
  } else if (order_batch) {
    MQThreadTaskSTMBatch_1_ordered<MQ_Type, descriptor, OPTSTM>(std::ref(graph), std::ref(wl), &stats[0], std::ref(prios), std::ref(detector_2), 0);
  } else if (batch) {
    MQThreadTaskSTMBatch_1<MQ_Type, descriptor, OPTSTM>(graph, wl, &stats[0], prios, std::ref(detector_2), 0);
  } else {
    MQThreadTaskSTM<MQ_Type, descriptor>(graph, wl, &stats[0], prios, std::ref(detector_2), 0);
  }
  
  for (std::thread*& worker : workers) {
    worker->join();
    delete worker;
  }

  auto end = std::chrono::high_resolution_clock::now();
  auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(end-begin).count();
  if (!wl.empty(me)) {
    std::cout << "not empty\n";
  }
  std::cout << "runtime_ms " << ms << "\n";

  wl.dump(me);

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
  //galois::runtime::reportStat_Single("BFS-MQBucket", "Iterations", totalIter);
  //galois::runtime::reportStat_Single("BFS-MQBucket", "EmptyWork", totalEmptyWork);
}

template<typename PQ_Type>
void spawnTasksPIPQ(PQ_Type& wl, Graph& graph, const GNode& source, int threadNum, std::atomic<uint32_t> *prios, termination_detector_2& detector) {
  stat_t stats[threadNum];
  wl.init_thread(0);
  wl.push(0, source);

  std::vector<std::thread*> workers;
  cpu_set_t cpuset;

  auto begin = std::chrono::high_resolution_clock::now();

  for (int i = 1; i < threadNum; i++) {
    CPU_ZERO(&cpuset);
    uint64_t coreID = pinning[i];
    CPU_SET(coreID, &cpuset);
    std::thread *newThread = new std::thread(
        MQThreadTaskPIPQ<PQ_Type>, i, std::ref(graph),
        std::ref(wl), &stats[i], std::ref(prios), std::ref(detector));
    
    int rc = pthread_setaffinity_np(newThread->native_handle(),
                                    sizeof(cpu_set_t), &cpuset);
    if (rc != 0) {
        std::cerr << "Error calling pthread_setaffinity_np: " << rc << "\n";
    }
    workers.push_back(newThread);
  }
  CPU_ZERO(&cpuset);
  CPU_SET(pinning[0], &cpuset);
  sched_setaffinity(0, sizeof(cpuset), &cpuset);

  MQThreadTaskPIPQ<PQ_Type>(0, std::ref(graph), std::ref(wl), &stats[0], std::ref(prios), std::ref(detector));
  
  for (std::thread*& worker : workers) {
    worker->join();
    delete worker;
  }

  auto end = std::chrono::high_resolution_clock::now();
  auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(end-begin).count();
  // if (!wl.empty()) { // TODO: implement empty().. oops lol
  //   std::cout << "not empty\n";
  // }
  //wl.stat();
  std::cout << "runtime_ms " << ms << "\n";

  //wl.dump();

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
  //galois::runtime::reportStat_Single("BFS-MQBucket", "Iterations", totalIter);
  //galois::runtime::reportStat_Single("BFS-MQBucket", "EmptyWork", totalEmptyWork);
}

template<typename PQ_Type>
void spawnTasksSMQ(PQ_Type& wl, Graph& graph, const GNode& source, int threadNum, std::atomic<uint32_t> *prios, termination_detector_2& detector) {
  stat_t stats[threadNum];
  wl.init_thread(0);
  wl.insert(0, source);

  std::vector<std::thread*> workers;
  cpu_set_t cpuset;

  auto begin = std::chrono::high_resolution_clock::now();

  for (int i = 1; i < threadNum; i++) {
    CPU_ZERO(&cpuset);
    uint64_t coreID = pinning[i];
    CPU_SET(coreID, &cpuset);
    std::thread *newThread = new std::thread(
        MQThreadTaskSMQ<PQ_Type>, i, std::ref(graph),
        std::ref(wl), &stats[i], std::ref(prios), std::ref(detector));
    
    int rc = pthread_setaffinity_np(newThread->native_handle(),
                                    sizeof(cpu_set_t), &cpuset);
    if (rc != 0) {
        std::cerr << "Error calling pthread_setaffinity_np: " << rc << "\n";
    }
    workers.push_back(newThread);
  }
  CPU_ZERO(&cpuset);
  CPU_SET(pinning[0], &cpuset);
  sched_setaffinity(0, sizeof(cpuset), &cpuset);

  MQThreadTaskSMQ<PQ_Type>(0, std::ref(graph), std::ref(wl), &stats[0], std::ref(prios), std::ref(detector));
  
  for (std::thread*& worker : workers) {
    worker->join();
    delete worker;
  }

  auto end = std::chrono::high_resolution_clock::now();
  auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(end-begin).count();
  // if (!wl.empty()) { // TODO: implement empty().. oops lol
  //   std::cout << "not empty\n";
  // }
  //wl.stat();
  std::cout << "runtime_ms " << ms << "\n";

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
  //galois::runtime::reportStat_Single("BFS-MQBucket", "Iterations", totalIter);
  //galois::runtime::reportStat_Single("BFS-MQBucket", "EmptyWork", totalEmptyWork);
}

template<typename PQ_Type>
void spawnTasksLinden(PQ_Type& wl, Graph& graph, const GNode& source, int threadNum, std::atomic<uint32_t> *prios, termination_detector_2& detector) {
  stat_t stats[threadNum];
  std::cout << "Inserting source\n";
  insert(wl, 0, source);
  std::cout << "Inserted.\n";

  std::vector<std::thread*> workers;
  cpu_set_t cpuset;

  auto begin = std::chrono::high_resolution_clock::now();

  for (int i = 1; i < threadNum; i++) {
    CPU_ZERO(&cpuset);
    uint64_t coreID = pinning[i];
    CPU_SET(coreID, &cpuset);
    std::thread *newThread = new std::thread(
        MQThreadTaskLinden<PQ_Type>, i, std::ref(graph),
        std::ref(wl), &stats[i], std::ref(prios), std::ref(detector));
    
    int rc = pthread_setaffinity_np(newThread->native_handle(),
                                    sizeof(cpu_set_t), &cpuset);
    if (rc != 0) {
        std::cerr << "Error calling pthread_setaffinity_np: " << rc << "\n";
    }
    workers.push_back(newThread);
  }
  CPU_ZERO(&cpuset);
  CPU_SET(pinning[0], &cpuset);
  sched_setaffinity(0, sizeof(cpuset), &cpuset);

  MQThreadTaskLinden<PQ_Type>(0, std::ref(graph), std::ref(wl), &stats[0], std::ref(prios), std::ref(detector));
  
  for (std::thread*& worker : workers) {
    worker->join();
    delete worker;
  }

  auto end = std::chrono::high_resolution_clock::now();
  auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(end-begin).count();
  // if (!wl.empty()) { // TODO: implement empty().. oops lol
  //   std::cout << "not empty\n";
  // }
  //wl.stat();
  std::cout << "runtime_ms " << ms << "\n";

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
  //galois::runtime::reportStat_Single("BFS-MQBucket", "Iterations", totalIter);
  //galois::runtime::reportStat_Single("BFS-MQBucket", "EmptyWork", totalEmptyWork);
}

template<typename PQ_Type>
void spawnTasksSpray(PQ_Type& wl, Graph& graph, const GNode& source, int threadNum, std::atomic<uint32_t> *prios, termination_detector_2& detector, thread_data_t *data) {
  stat_t stats[threadNum];
  std::cout << "Inserting source\n";
  //sl_add_val(wl, 0, src, 0); // todo: diff b/w (fraser) insert(), and sl_add_val() ??
  fraser_insert(wl, 0, source);
  std::cout << "Inserted.\n";

  std::vector<std::thread*> workers;
  cpu_set_t cpuset;
  auto begin = std::chrono::high_resolution_clock::now();

  for (int i = 1; i < threadNum; i++) {
    CPU_ZERO(&cpuset);
    uint64_t coreID = pinning[i];
    CPU_SET(coreID, &cpuset);
    std::thread *newThread = new std::thread(
        MQThreadTaskSpray<PQ_Type>, i, std::ref(graph),
        std::ref(wl), &stats[i], std::ref(prios), std::ref(detector), data);
    
    int rc = pthread_setaffinity_np(newThread->native_handle(),
                                    sizeof(cpu_set_t), &cpuset);
    if (rc != 0) {
        std::cerr << "Error calling pthread_setaffinity_np: " << rc << "\n";
    }
    workers.push_back(newThread);
  }
  CPU_ZERO(&cpuset);
  CPU_SET(pinning[0], &cpuset);
  sched_setaffinity(0, sizeof(cpuset), &cpuset);

  MQThreadTaskSpray<PQ_Type>(0, std::ref(graph), std::ref(wl), &stats[0], std::ref(prios), std::ref(detector), data);
  
  for (std::thread*& worker : workers) {
    worker->join();
    delete worker;
  }

  auto end = std::chrono::high_resolution_clock::now();
  auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(end-begin).count();
  // if (!wl.empty()) { // TODO: implement empty().. oops lol
  //   std::cout << "not empty\n";
  // }
  //wl.stat();
  std::cout << "runtime_ms " << ms << "\n";

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
  //galois::runtime::reportStat_Single("BFS-MQBucket", "Iterations", totalIter);
  //galois::runtime::reportStat_Single("BFS-MQBucket", "EmptyWork", totalEmptyWork);
}

template<bool usePrefetch=true>
void MQAlgo(Graph& graph, GNode source, int threadNum, int queueNum) {
  std::cout << "threads = " << threadNum << "\n";
  std::cout << "queues = " << queueNum << "\n";
  std::cout << "batchSizePop = " << batch1 << "\n";
  std::cout << "batchSizePush = " << batch2 << "\n";
  std::cout << "stickiness = " << stickiness << "\n";
  std::cout << "prefetch " << usePrefetch << "\n";

  termination_detector_2 detector_2(threadNum);

  // The distance array that records the latest distances
  std::atomic<uint32_t> *prios = new std::atomic<uint32_t>[graph.size()];
  for (uint i = 0; i < graph.size(); i++) {
    prios[i].store(UINT32_MAX, std::memory_order_relaxed);
  }
  prios[source] = 0;
  graph.getData(source) = 0;

  // Prefetcher lambda for reducing cache misses on load to a 
  // vertex's distance
  auto prefetcher = [&] (uint32_t v) -> void {
    // priority of this node
    __builtin_prefetch(&prios[v], 0, 3);

    // the first and last of edges
    graph.prefetchEdgeStart(v);
    graph.prefetchEdgeEnd(v);
  };

  if (algo == MQ) {
    using MQ_Type = mbq::MultiQueue<decltype(prefetcher), std::greater<PQElement>, uint32_t, uint32_t, usePrefetch>;
    MQ_Type wl(prefetcher, queueNum, threadNum, batch1, batch2, stickiness);
    spawnTasks<MQ_Type>(wl, graph, source, threadNum, prios, detector_2);

  } else if (algo == MQBucket) {
    // MQBucket
    std::cout << "buckets = " << bucketNum << "\n";
    std::cout << "delta = " << stepShift << "\n";

    // Lambda for mapping a priority to a priority level
    auto getBucketID = [&] (uint32_t v) -> mbq::BucketID {
      uint32_t d = prios[v].load(std::memory_order_acquire);
      return (d >> stepShift);
    };

    using MQ_Bucket_Type = mbq::MultiBucketQueue<decltype(getBucketID), decltype(prefetcher), std::greater<mbq::BucketID>, uint32_t, uint32_t, usePrefetch>;
    MQ_Bucket_Type wl(getBucketID, prefetcher, queueNum, threadNum, stepShift, bucketNum, batch1, batch2, mbq::increasing, stickiness);
    spawnTasks<MQ_Bucket_Type>(wl, graph, source, threadNum, prios, detector_2);
  } else if (algo == SkipHashPQ) {
    // TODO: add prefetcher ??

    // SkipHashPQ
    #define _ALG eager_noext_c1_t
    #define _OREC orec_po_t
    #define _CLOCK rdtscp_clock_t
    //#define _CLOCK gv1_clock_t

    using descriptor = _ALG<_OREC, clock_policy::_CLOCK>;
    using OPTSTM = _ALG<_OREC, clock_policy::_CLOCK>;
    using map = skiphash_pq_relaxed<uint32_t, uint32_t, OPTSTM>;
    auto *me = new descriptor();

    config_t cfg;
    cfg.max_levels = sl_max_levels;
    cfg.buckets = buckets;
    cfg.chunksize = chunksize;
    cfg.num_queues = num_chunks;
    cfg.max_batch_size = max_batch_size;
    cfg.delta = stepShift;

    std::cout << "cfg.max_levels: " << static_cast<int>(cfg.max_levels) << "\n";
    std::cout << "cfg.buckets: " << cfg.buckets << "\n";
    std::cout << "cfg.chunksize: " << cfg.chunksize << "\n";
    std::cout << "cfg.num_queues: " << cfg.num_queues << "\n";
    std::cout << "cfg.max_batch_size: " << cfg.max_batch_size << "\n";
    std::cout << "cfg.delta: " << cfg.delta << "\n";
    
    map pq(me, &cfg);
    spawnTasksSTM<map, descriptor, OPTSTM>(pq, graph, source, threadNum, prios, detector_2);
    //else spawnTasksSTM<map, descriptor, OPTSTM>(pq, graph, source, threadNum, prios, detector, detector_2);
  } else if (algo == PIPQ) {
    int heap_list_size = 10000000;
    int cntr_tsh = 10;
    int cntr_max = 100;

    using pipq_t = pq_ns::pipq<uint32_t>;

    //std::cout << "counter max: " << counter_max << ", counter tsh: " << counter_tsh << "\n";
    auto numa_pq_ds = pipq_t(heap_list_size, threadNum, cntr_tsh, cntr_max, pinning);
    numa_pq_ds.PQInit();
    spawnTasksPIPQ<pipq_t>(numa_pq_ds, graph, source, threadNum, prios, detector_2);
  } else if (algo == SMQ) {
    // change STEAL_PROB
    if (steal_prob == 8 && steal_size == 8) {
      const size_t steal_probability = 8; // 1/8 probability of stealing
      const size_t steal_batch_size = 8; // size of batch to steal

      using smq_t = smq_ns::StealingMultiQueue<std::pair<uint32_t,uint32_t>,uint32_t,steal_probability,steal_batch_size,true>;
      auto smq_ds = smq_t(threadNum);
      spawnTasksSMQ<smq_t>(smq_ds, graph, source, threadNum, prios, detector_2);
    } else if (steal_prob == 8 && steal_size == 32) {
      const size_t steal_probability = 8; // 1/8 probability of stealing
      const size_t steal_batch_size = 32; // size of batch to steal

      using smq_t = smq_ns::StealingMultiQueue<std::pair<uint32_t,uint32_t>,uint32_t,steal_probability,steal_batch_size,true>;
      auto smq_ds = smq_t(threadNum);
      spawnTasksSMQ<smq_t>(smq_ds, graph, source, threadNum, prios, detector_2);
    } else if (steal_prob == 8 && steal_size == 128) {
      const size_t steal_probability = 8; // 1/8 probability of stealing
      const size_t steal_batch_size = 128; // size of batch to steal

      using smq_t = smq_ns::StealingMultiQueue<std::pair<uint32_t,uint32_t>,uint32_t,steal_probability,steal_batch_size,true>;
      auto smq_ds = smq_t(threadNum);
      spawnTasksSMQ<smq_t>(smq_ds, graph, source, threadNum, prios, detector_2);
    } else if (steal_prob == 16 && steal_size == 128) {
      const size_t steal_probability = 16; // 1/8 probability of stealing
      const size_t steal_batch_size = 128; // size of batch to steal

      using smq_t = smq_ns::StealingMultiQueue<std::pair<uint32_t,uint32_t>,uint32_t,steal_probability,steal_batch_size,true>;
      auto smq_ds = smq_t(threadNum);
      spawnTasksSMQ<smq_t>(smq_ds, graph, source, threadNum, prios, detector_2);
    } else if (steal_prob == 32 && steal_size == 128) {
      const size_t steal_probability = 32; // 1/8 probability of stealing
      const size_t steal_batch_size = 128; // size of batch to steal

      using smq_t = smq_ns::StealingMultiQueue<std::pair<uint32_t,uint32_t>,uint32_t,steal_probability,steal_batch_size,true>;
      auto smq_ds = smq_t(threadNum);
      spawnTasksSMQ<smq_t>(smq_ds, graph, source, threadNum, prios, detector_2);
    }
    // change STEAL_SIZE
    else if (steal_prob == 16 && steal_size == 16) {
      const size_t steal_probability = 16; // 1/8 probability of stealing
      const size_t steal_batch_size = 64; // size of batch to steal

      using smq_t = smq_ns::StealingMultiQueue<std::pair<uint32_t,uint32_t>,uint32_t,steal_probability,steal_batch_size,true>;
      auto smq_ds = smq_t(threadNum);
      spawnTasksSMQ<smq_t>(smq_ds, graph, source, threadNum, prios, detector_2);
    } else if (steal_prob == 16 && steal_size == 32) {
      const size_t steal_probability = 16; // 1/8 probability of stealing
      const size_t steal_batch_size = 32; // size of batch to steal

      using smq_t = smq_ns::StealingMultiQueue<std::pair<uint32_t,uint32_t>,uint32_t,steal_probability,steal_batch_size,true>;
      auto smq_ds = smq_t(threadNum);
      spawnTasksSMQ<smq_t>(smq_ds, graph, source, threadNum, prios, detector_2);
    } else if (steal_prob == 16 && steal_size == 64) {
      const size_t steal_probability = 16; // 1/8 probability of stealing
      const size_t steal_batch_size = 64; // size of batch to steal

      using smq_t = smq_ns::StealingMultiQueue<std::pair<uint32_t,uint32_t>,uint32_t,steal_probability,steal_batch_size,true>;
      auto smq_ds = smq_t(threadNum);
      spawnTasksSMQ<smq_t>(smq_ds, graph, source, threadNum, prios, detector_2);
    } else if (steal_prob == 16 && steal_size == 256) {
      const size_t steal_probability = 16; // 1/8 probability of stealing
      const size_t steal_batch_size = 256; // size of batch to steal

      using smq_t = smq_ns::StealingMultiQueue<std::pair<uint32_t,uint32_t>,uint32_t,steal_probability,steal_batch_size,true>;
      auto smq_ds = smq_t(threadNum);
      spawnTasksSMQ<smq_t>(smq_ds, graph, source, threadNum, prios, detector_2);
    } else {
      std::cout << "ERROR: steal_prob,steal_size setup not enabled\n";
      std::terminate();
    }
  } else if (algo == Linden) {
    int max_offset = 32;
    _init_gc_subsystem();
    pq_t * linden_pq = pq_init(max_offset);
    spawnTasksLinden(linden_pq, graph, source, threadNum, prios, detector_2);
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
    spawnTasksSpray(sl_spray, graph, source, threadNum, prios, detector_2, data);
  } else {
    std::cout << "ERROR: invalid data structure\n";
    std::terminate();
  }
  delete[] prios;
}

template <bool CONCURRENT>
void runAlgo(Graph& graph, const GNode& source) {

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
  case OBIM:
    std::cout << "running OBIM with chunk size " << chunk << "\n";
    switch (chunk) {
      case 4:
        OBIMAlgo<CONCURRENT, UpdateRequest, OBIM4>(graph, source, ReqPushWrap(), OutEdgeRangeFn{graph});
        break;
      case 8:
        OBIMAlgo<CONCURRENT, UpdateRequest, OBIM8>(graph, source, ReqPushWrap(), OutEdgeRangeFn{graph});
        break;
      case 16:
        OBIMAlgo<CONCURRENT, UpdateRequest, OBIM16>(graph, source, ReqPushWrap(), OutEdgeRangeFn{graph});
        break;
      case 32:
        OBIMAlgo<CONCURRENT, UpdateRequest, OBIM32>(graph, source, ReqPushWrap(), OutEdgeRangeFn{graph});
        break;
      case 64:
        OBIMAlgo<CONCURRENT, UpdateRequest, OBIM64>(graph, source, ReqPushWrap(), OutEdgeRangeFn{graph});
        break;
      case 128:
        OBIMAlgo<CONCURRENT, UpdateRequest, OBIM128>(graph, source, ReqPushWrap(), OutEdgeRangeFn{graph});
        break;
      case 256:
        OBIMAlgo<CONCURRENT, UpdateRequest, OBIM256>(graph, source, ReqPushWrap(), OutEdgeRangeFn{graph});
        break;
      default:
        std::cerr << "ERROR: unkown chunk size\n";
    }
    break;
  case PMOD:
    std::cout << "running PMOD with chunk size " << chunk << "\n";
    switch (chunk) {
      case 4:
        OBIMAlgo<CONCURRENT, UpdateRequest, PMOD4>(graph, source, ReqPushWrap(), OutEdgeRangeFn{graph});
        break;
      case 8:
        OBIMAlgo<CONCURRENT, UpdateRequest, PMOD8>(graph, source, ReqPushWrap(), OutEdgeRangeFn{graph});
        break;
      case 16:
        OBIMAlgo<CONCURRENT, UpdateRequest, PMOD16>(graph, source, ReqPushWrap(), OutEdgeRangeFn{graph});
        break;
      case 32:
        OBIMAlgo<CONCURRENT, UpdateRequest, PMOD32>(graph, source, ReqPushWrap(), OutEdgeRangeFn{graph});
        break;
      case 64:
        OBIMAlgo<CONCURRENT, UpdateRequest, PMOD64>(graph, source, ReqPushWrap(), OutEdgeRangeFn{graph});
        break;
      case 128:
        OBIMAlgo<CONCURRENT, UpdateRequest, PMOD128>(graph, source, ReqPushWrap(), OutEdgeRangeFn{graph});
        break;
      case 256:
        OBIMAlgo<CONCURRENT, UpdateRequest, PMOD256>(graph, source, ReqPushWrap(), OutEdgeRangeFn{graph});
        break;
      default:
        std::cerr << "ERROR: unkown chunk size\n";
    }
    break;
  case MQBucket:
    std::cout << "running MQBucket\n";
    if (prefetch == 1) MQAlgo<true>(graph, source, threadNum, queueNum); 
    else MQAlgo<false>(graph, source, threadNum, queueNum); 
    break;
  case MQ:
    std::cout << "running MQ\n";
    if (prefetch == 1) MQAlgo<true>(graph, source, threadNum, queueNum); 
    else MQAlgo<false>(graph, source, threadNum, queueNum); 
    break;
  case SkipHashPQ:
    std::cout << "running SkipHashPQ\n";
    MQAlgo<false>(graph, source, threadNum, queueNum);
    break;
  case PIPQ:
    std::cout << "running PIPQ\n";
    MQAlgo<false>(graph, source, threadNum, queueNum);
    break;
  case SMQ:
    std::cout << "running SMQ\n";
    MQAlgo<false>(graph, source, threadNum, queueNum);
    break;
  case Linden:
    std::cout << "running Linden\n";
    MQAlgo<false>(graph, source, threadNum, queueNum);
    break;
  case Spray:
    std::cout << "running Spraylist\n";
    MQAlgo<false>(graph, source, threadNum, queueNum);
    break;
  default:
    std::cerr << "ERROR: unknown algo type\n";
  }
}

int main(int argc, char** argv) {
  galois::SharedMemSys G;
  LonestarStart(argc, argv, name, desc, url, &inputFile);

  galois::StatTimer totalTime("TimerTotal");
  totalTime.start();

  Graph graph;
  GNode source;
  GNode report;

  std::cout << "Reading from file: " << inputFile << "\n";
  galois::graphs::readGraph(graph, inputFile);
  std::cout << "Read " << graph.size() << " nodes, " << graph.sizeEdges()
            << " edges\n";

  if (startNode >= graph.size() || reportNode >= graph.size()) {
    std::cerr << "failed to set report: " << reportNode
              << " or failed to set source: " << startNode << "\n";
    abort();
  }

  auto it = graph.begin();
  std::advance(it, startNode.getValue());
  source = *it;
  it     = graph.begin();
  std::advance(it, reportNode.getValue());
  report = *it;

  size_t approxNodeData = 4 * (graph.size() + graph.sizeEdges());
  galois::preAlloc(8 * numThreads +
                   approxNodeData / galois::runtime::pagePoolSize());

  galois::reportPageAlloc("MeminfoPre");

  galois::do_all(galois::iterate(graph),
                 [&graph](GNode n) { graph.getData(n) = BFS::DIST_INFINITY; });
  graph.getData(source) = 0;

  // 1. Initialize the thread pool for async logging
  // Queue size: 8192 items, 1 backing thread
  //spdlog::init_thread_pool(8192, 1);

  // 2. Create an asynchronous logger
  //auto async_file = spdlog::basic_logger_mt<spdlog::async_factory>(
  //    "async_logger", "logs/performance.txt"
  //);
  //spdlog::set_default_logger(async_file);

  std::cout << "Running " << ALGO_NAMES[algo] << " algorithm with "
            << (bool(execution) ? "PARALLEL" : "SERIAL") << " execution\n";

  galois::StatTimer execTime("Timer_0");
  execTime.start();

  if (execution == SERIAL) {
    runAlgo<false>(graph, source);
  } else if (execution == PARALLEL) {
    runAlgo<true>(graph, source);
  } else {
    std::cerr << "ERROR: unknown type of execution passed to -exec\n";
  }

  execTime.stop();

  galois::reportPageAlloc("MeminfoPost");

  std::cout << "Node " << reportNode << " has distance "
            << graph.getData(report) << "\n";

  // Sanity checking code
  galois::GReduceMax<uint64_t> maxDistance;
  galois::GAccumulator<uint64_t> distanceSum;
  galois::GAccumulator<uint32_t> visitedNode;
  maxDistance.reset();
  distanceSum.reset();
  visitedNode.reset();

  galois::do_all(
      galois::iterate(graph),
      [&](uint64_t i) {
        uint32_t myDistance = graph.getData(i);

        if (myDistance != BFS::DIST_INFINITY) {
          maxDistance.update(myDistance);
          distanceSum += myDistance;
          visitedNode += 1;
        }
      },
      galois::loopname("Sanity check"), galois::no_stats());

  // report sanity stats
  uint64_t rMaxDistance = maxDistance.reduce();
  uint64_t rDistanceSum = distanceSum.reduce();
  uint64_t rVisitedNode = visitedNode.reduce();
  galois::gInfo("# visited nodes is ", rVisitedNode);
  galois::gInfo("Max distance is ", rMaxDistance);
  galois::gInfo("Sum of visited distances is ", rDistanceSum);

  if (!skipVerify) {
    if (BFS::verify(graph, source)) {
      std::cout << "Verification successful.\n";
    } else {
      GALOIS_DIE("verification failed");
    }
  }

  totalTime.stop();
  //spdlog::shutdown();

  return 0;
}
