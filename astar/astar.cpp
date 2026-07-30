#include <algorithm>
#include <cmath>
#include <fstream>
#include <iostream>
#include <fstream>
#include <stdlib.h>
#include <tuple>
#include <vector>
#include <cassert>
#include <thread>
#include <time.h>
#include <atomic>

#include "include/MultiBucketQueue.h"
#include "include/BucketStructs.h"
#include "include/MultiQueue.h"
#include "util.h"

#include "../galois/lonestar/analytics/cpu/config.h"
#include <include-skiphashpq/optstm2/eager_noext_c1.h>
#include <include-skiphashpq/skiphash_pq_relaxed.h>

#include <include-pipq/pipq_impl.h>
#include <include-smq/smq_impl.h>
#include <include-linden/linden.h>
#include "gc/ptst.h"
#include "intset.h" // spraylist include
#include "include/random.h" // spraylist include

#include "../priority_tracker.h"

#include "../galois/lonestar/analytics/cpu/termination_helper.h"

const int pinning[96] = {0,2,4,6,8,10,12,14,16,18,20,22,24,26,28,30,32,34,36,38,40,42,44,46,1,3,5,7,9,11,13,15,17,19,21,23,25,27,29,31,33,35,37,39,41,43,45,47,48,50,52,54,56,58,60,62,64,66,68,70,72,74,76,78,80,82,84,86,88,90,92,94,49,51,53,55,57,59,61,63,65,67,69,71,73,75,77,79,81,83,85,87,89,91,93,95};


/* dsm: A*-search for road maps. Conventions:
 *  - Node positions are in (lat, lon) format
 *  - All latitudes, longitudes, and distances are in radians unless specified
 *    (distances are sometimes stored in cm, with vars having suffix _cm)
 *  - Distances are great-circle distances
 *  - Adjacencies include precomputed distances to reduce the number of intermediate points in long roads
 *  - Adjacencies are symmetric (all roads are considered 2-way) and the graph must be strongly connected
 *  - Algorithm finds minimum-distance (not minimum-time) route (we'd need road speeds for minimum time,
 *    but it's pretty doable)
 */

/* The parallel versions use 64 bit data to allow for single CAS of a 64 bit data.
 * It is a union of 2 x 32 bit that encapsulates parent and distance.
 *  | 63..32 | 31..0  |
 *  | parent | fscore |
 */

constexpr static uint64_t FSCORE_MASK = 0xffffffff;

OPTSTM2_GLOBALS_INITIALIZER;

alignas(64) extern uint8_t levelmax[64];
__thread unsigned long *seeds;

using PQElement = std::tuple<uint32_t, uint32_t>;
struct stat_t {
  uint32_t iter = 0;
  uint32_t emptyWork = 0;
};

template<typename MQ>
void MQThreadTask(const Vertex* graph, MQ &wl, stat_t *stats,
                    std::atomic<uint64_t> *data, 
                    uint32_t sourceNode, uint32_t targetNode)
{
    uint32_t iter = 0UL;
    uint32_t emptyWork = 0UL;
    uint32_t gScore;
    uint32_t src;
    wl.initTID();

    while (true) {
        auto item = wl.pop();
        if (item) std::tie(gScore, src) = item.get();
        else break;

        uint64_t targetData = data[targetNode].load(std::memory_order_relaxed);
        uint32_t targetDist = targetData & FSCORE_MASK;
        ++iter;

        // With the astar definition, our heuristic
        // will always overestimate. If the current task's
        // gScore is already greater than the targetDist,
        // then it won't ever lead to a shorter path to target.
        if (targetDist <= gScore) {
            ++emptyWork;
            continue;
        }

        uint64_t srcData = data[src].load(std::memory_order_relaxed);
        uint32_t fScore = srcData & FSCORE_MASK;

        for (uint32_t e = 0; e < graph[src].adj.size(); e++) {
            auto& adjNode = graph[src].adj[e];
            uint32_t dst = adjNode.n;
            uint32_t nFScore = fScore + adjNode.d_cm;
            if (targetDist <= nFScore) continue;
            uint64_t dstData = data[dst].load(std::memory_order_relaxed);

            // try CAS the neighbor with the new actual distance
            bool swapped = false;
            do {
                uint32_t dstDist = dstData & FSCORE_MASK;
                if (dstDist <= nFScore) break;
                uint64_t srcShift = src;
                uint64_t swapVal = (srcShift << 32) | nFScore;
                swapped = data[dst].compare_exchange_weak(
                    dstData, swapVal,
                    std::memory_order_acq_rel,
                    std::memory_order_acquire);
            } while(!swapped);
            if (!swapped) continue;

            // compute new heuristic of the neighbor
            uint32_t nGScore = std::max(gScore, nFScore + dist(&graph[dst], &graph[targetNode]));
            if (targetDist <= nGScore) continue;

            // only push if relaxing this vertex is profitable
            wl.push(nGScore, dst);
        }
    }

    stats->iter = iter;
    stats->emptyWork = emptyWork;
}

template<typename MQ>
void ThreadTaskSMQ(int tid, const Vertex* graph, MQ &wl, stat_t *stats,
                    std::atomic<uint64_t> *data, 
                    uint32_t sourceNode, uint32_t targetNode, termination_detector_2& detector)
{
    uint32_t iter = 0UL;
    uint32_t emptyWork = 0UL;
    uint32_t gScore;
    uint32_t src;
    using extract_ret_t = std::optional<std::pair<uint,uint>>;
    auto call_extract = [&]() {
        return wl.extract_min();
    };
    if (tid) wl.init_thread(tid);

    while (true) {
        auto ret_term = try_extract<extract_ret_t>(detector, call_extract); // handles termination detection
        if (!ret_term) break; // TERMINATE
        std::tie(gScore, src) = ret_term.value();

        uint64_t targetData = data[targetNode].load(std::memory_order_relaxed);
        uint32_t targetDist = targetData & FSCORE_MASK;
        ++iter;

        // With the astar definition, our heuristic
        // will always overestimate. If the current task's
        // gScore is already greater than the targetDist,
        // then it won't ever lead to a shorter path to target.
        if (targetDist <= gScore) {
            ++emptyWork;
            continue;
        }

        uint64_t srcData = data[src].load(std::memory_order_relaxed);
        uint32_t fScore = srcData & FSCORE_MASK;

        for (uint32_t e = 0; e < graph[src].adj.size(); e++) {
            auto& adjNode = graph[src].adj[e];
            uint32_t dst = adjNode.n;
            uint32_t nFScore = fScore + adjNode.d_cm;
            if (targetDist <= nFScore) continue;
            uint64_t dstData = data[dst].load(std::memory_order_relaxed);

            // try CAS the neighbor with the new actual distance
            bool swapped = false;
            do {
                uint32_t dstDist = dstData & FSCORE_MASK;
                if (dstDist <= nFScore) break;
                uint64_t srcShift = src;
                uint64_t swapVal = (srcShift << 32) | nFScore;
                swapped = data[dst].compare_exchange_weak(
                    dstData, swapVal,
                    std::memory_order_acq_rel,
                    std::memory_order_acquire);
            } while(!swapped);
            if (!swapped) continue;

            // compute new heuristic of the neighbor
            uint32_t nGScore = std::max(gScore, nFScore + dist(&graph[dst], &graph[targetNode]));
            if (targetDist <= nGScore) continue;

            // only push if relaxing this vertex is profitable
            wl.insert(nGScore, dst);
        }
    }

    stats->iter = iter;
    stats->emptyWork = emptyWork;
}

template<typename MQ>
void ThreadTaskPIPQ(int tid, const Vertex* graph, MQ &wl, stat_t *stats,
                    std::atomic<uint64_t> *data, 
                    uint32_t sourceNode, uint32_t targetNode, termination_detector_2& detector)
{
    uint32_t iter = 0UL;
    uint32_t emptyWork = 0UL;
    uint32_t gScore;
    uint32_t src;
    using extract_ret_t = std::optional<std::pair<uint,uint>>;
    auto call_extract = [&]() {
        return wl.extract_min();
    };
    if (tid) wl.init_thread(tid);

    while (true) {
        auto ret_term = try_extract<extract_ret_t>(detector, call_extract); // handles termination detection
        if (!ret_term) break; // TERMINATE
        std::tie(gScore, src) = ret_term.value();

        uint64_t targetData = data[targetNode].load(std::memory_order_relaxed);
        uint32_t targetDist = targetData & FSCORE_MASK;
        ++iter;

        // With the astar definition, our heuristic
        // will always overestimate. If the current task's
        // gScore is already greater than the targetDist,
        // then it won't ever lead to a shorter path to target.
        if (targetDist <= gScore) {
            ++emptyWork;
            continue;
        }

        uint64_t srcData = data[src].load(std::memory_order_relaxed);
        uint32_t fScore = srcData & FSCORE_MASK;

        for (uint32_t e = 0; e < graph[src].adj.size(); e++) {
            auto& adjNode = graph[src].adj[e];
            uint32_t dst = adjNode.n;
            uint32_t nFScore = fScore + adjNode.d_cm;
            if (targetDist <= nFScore) continue;
            uint64_t dstData = data[dst].load(std::memory_order_relaxed);

            // try CAS the neighbor with the new actual distance
            bool swapped = false;
            do {
                uint32_t dstDist = dstData & FSCORE_MASK;
                if (dstDist <= nFScore) break;
                uint64_t srcShift = src;
                uint64_t swapVal = (srcShift << 32) | nFScore;
                swapped = data[dst].compare_exchange_weak(
                    dstData, swapVal,
                    std::memory_order_acq_rel,
                    std::memory_order_acquire);
            } while(!swapped);
            if (!swapped) continue;

            // compute new heuristic of the neighbor
            uint32_t nGScore = std::max(gScore, nFScore + dist(&graph[dst], &graph[targetNode]));
            if (targetDist <= nGScore) continue;

            // only push if relaxing this vertex is profitable
            wl.push(nGScore, dst);
        }
    }

    stats->iter = iter;
    stats->emptyWork = emptyWork;
}

template<typename MQ>
void ThreadTaskLinden(const Vertex* graph, MQ &wl, stat_t *stats,
                    std::atomic<uint64_t> *data, 
                    uint32_t sourceNode, uint32_t targetNode, termination_detector_2& detector)
{
    uint32_t iter = 0UL;
    uint32_t emptyWork = 0UL;
    uint32_t gScore;
    uint32_t src;
    using extract_ret_t = std::optional<std::pair<uint,uint>>;
    auto call_extract = [&]() {
        return extract_min(wl);
    };

    while (true) {
        auto ret_term = try_extract<extract_ret_t>(detector, call_extract); // handles termination detection
        if (!ret_term) break; // TERMINATE
        std::tie(gScore, src) = ret_term.value();

        uint64_t targetData = data[targetNode].load(std::memory_order_relaxed);
        uint32_t targetDist = targetData & FSCORE_MASK;
        ++iter;

        // With the astar definition, our heuristic
        // will always overestimate. If the current task's
        // gScore is already greater than the targetDist,
        // then it won't ever lead to a shorter path to target.
        if (targetDist <= gScore) {
            ++emptyWork;
            continue;
        }

        uint64_t srcData = data[src].load(std::memory_order_relaxed);
        uint32_t fScore = srcData & FSCORE_MASK;

        for (uint32_t e = 0; e < graph[src].adj.size(); e++) {
            auto& adjNode = graph[src].adj[e];
            uint32_t dst = adjNode.n;
            uint32_t nFScore = fScore + adjNode.d_cm;
            if (targetDist <= nFScore) continue;
            uint64_t dstData = data[dst].load(std::memory_order_relaxed);

            // try CAS the neighbor with the new actual distance
            bool swapped = false;
            do {
                uint32_t dstDist = dstData & FSCORE_MASK;
                if (dstDist <= nFScore) break;
                uint64_t srcShift = src;
                uint64_t swapVal = (srcShift << 32) | nFScore;
                swapped = data[dst].compare_exchange_weak(
                    dstData, swapVal,
                    std::memory_order_acq_rel,
                    std::memory_order_acquire);
            } while(!swapped);
            if (!swapped) continue;

            // compute new heuristic of the neighbor
            uint32_t nGScore = std::max(gScore, nFScore + dist(&graph[dst], &graph[targetNode]));
            if (targetDist <= nGScore) continue;

            // only push if relaxing this vertex is profitable
            insert(wl, nGScore, dst);
        }
    }

    stats->iter = iter;
    stats->emptyWork = emptyWork;
}

template<typename MQ>
void ThreadTaskSpray(const Vertex* graph, MQ &wl, stat_t *stats,
                    std::atomic<uint64_t> *data, 
                    uint32_t sourceNode, uint32_t targetNode,
                    termination_detector_2& detector,
                    thread_data_t *data_spray, int tid)
{
    uint32_t iter = 0UL;
    uint32_t emptyWork = 0UL;
    uint32_t gScore;
    uint32_t src;
    thread_data_t* data_item = &(data_spray[tid]);
    if (tid) {
        seeds = seed_rand();
    }
    using extract_ret_t = std::optional<std::pair<uint,uint>>;
    auto call_extract = [&]() {
        return spray_delete_min_key(wl, data_item);
    };

    while (true) {
        auto ret_term = try_extract<extract_ret_t>(detector, call_extract); // handles termination detection
        if (!ret_term) break; // TERMINATE
        std::tie(gScore, src) = ret_term.value();

        uint64_t targetData = data[targetNode].load(std::memory_order_relaxed);
        uint32_t targetDist = targetData & FSCORE_MASK;
        ++iter;

        // With the astar definition, our heuristic
        // will always overestimate. If the current task's
        // gScore is already greater than the targetDist,
        // then it won't ever lead to a shorter path to target.
        if (targetDist <= gScore) {
            ++emptyWork;
            continue;
        }

        uint64_t srcData = data[src].load(std::memory_order_relaxed);
        uint32_t fScore = srcData & FSCORE_MASK;

        for (uint32_t e = 0; e < graph[src].adj.size(); e++) {
            auto& adjNode = graph[src].adj[e];
            uint32_t dst = adjNode.n;
            uint32_t nFScore = fScore + adjNode.d_cm;
            if (targetDist <= nFScore) continue;
            uint64_t dstData = data[dst].load(std::memory_order_relaxed);

            // try CAS the neighbor with the new actual distance
            bool swapped = false;
            do {
                uint32_t dstDist = dstData & FSCORE_MASK;
                if (dstDist <= nFScore) break;
                uint64_t srcShift = src;
                uint64_t swapVal = (srcShift << 32) | nFScore;
                swapped = data[dst].compare_exchange_weak(
                    dstData, swapVal,
                    std::memory_order_acq_rel,
                    std::memory_order_acquire);
            } while(!swapped);
            if (!swapped) continue;

            // compute new heuristic of the neighbor
            uint32_t nGScore = std::max(gScore, nFScore + dist(&graph[dst], &graph[targetNode]));
            if (targetDist <= nGScore) continue;

            // only push if relaxing this vertex is profitable
            fraser_insert(wl, nGScore, dst);
        }
    }

    stats->iter = iter;
    stats->emptyWork = emptyWork;
}

std::atomic<int> active_elems(1); // source node to start

template<typename PQ, typename descriptor>
void MQThreadTaskSTM(const Vertex* graph, PQ &wl, stat_t *stats,
                    std::atomic<uint64_t> *data, 
                    uint32_t sourceNode, uint32_t targetNode,
                    std::map<int,int> *prios,
                    termination_detector_2& detector, int tid) {
    uint32_t iter = 0UL;
    uint32_t emptyWork = 0UL;
    uint32_t gScore;
    uint32_t src;
    auto* me = new descriptor();

    if (tid) wl.init_thread(me, tid);
    else wl.re_init_thread(me);

    using extract_ret_t = std::optional<std::pair<uint32_t,uint32_t>>;
    auto call_extract = [&]() {
        me->op_begin();
        auto ret = wl.extract_min(me);
        me->op_end();
        return ret;
    };

    while (true) {
        auto ret_term = try_extract<extract_ret_t>(detector, call_extract); // handles termination detection
        if (!ret_term) break; // TERMINATE
        std::tie(gScore, src) = ret_term.value();

        uint64_t targetData = data[targetNode].load(std::memory_order_relaxed);
        uint32_t targetDist = targetData & FSCORE_MASK;
        ++iter;

        // With the astar definition, our heuristic
        // will always overestimate. If the current task's
        // gScore is already greater than the targetDist,
        // then it won't ever lead to a shorter path to target.
        if (targetDist <= gScore) {
            ++emptyWork;
            continue;
        }

        uint64_t srcData = data[src].load(std::memory_order_relaxed);
        uint32_t fScore = srcData & FSCORE_MASK;

        for (uint32_t e = 0; e < graph[src].adj.size(); e++) {
            auto& adjNode = graph[src].adj[e];
            uint32_t dst = adjNode.n;
            uint32_t nFScore = fScore + adjNode.d_cm;
            if (targetDist <= nFScore) continue;
            uint64_t dstData = data[dst].load(std::memory_order_relaxed);

            // try CAS the neighbor with the new actual distance
            bool swapped = false;
            do {
                uint32_t dstDist = dstData & FSCORE_MASK;
                if (dstDist <= nFScore) break;
                uint64_t srcShift = src;
                uint64_t swapVal = (srcShift << 32) | nFScore;
                swapped = data[dst].compare_exchange_weak(
                    dstData, swapVal,
                    std::memory_order_acq_rel,
                    std::memory_order_acquire);
            } while(!swapped);
            if (!swapped) continue;

            // compute new heuristic of the neighbor
            uint32_t nGScore = std::max(gScore, nFScore + dist(&graph[dst], &graph[targetNode]));
            if (targetDist <= nGScore) continue;

            // only push if relaxing this vertex is profitable
            me->op_begin();
            wl.insert(me, nGScore, dst);
            me->op_end();
            //active_elems++;
        }
    }

    //std::cout << "exiting..." << std::this_thread::get_id() << "\n";

    stats->iter = iter;
    stats->emptyWork = emptyWork;
}

template<typename PQ, typename descriptor>
void MQThreadTaskSTM_Strict(const Vertex* graph, PQ &wl, stat_t *stats,
                    std::atomic<uint64_t> *data, 
                    uint32_t sourceNode, uint32_t targetNode,
                    std::map<int,int> *prios,
                    termination_detector_2& detector,
                    prio_tracker& p_tracker, int tid) {
    uint32_t iter = 0UL;
    uint32_t emptyWork = 0UL;
    uint32_t gScore;
    uint32_t src;
    auto* me = new descriptor();
    me->op_begin();
    if (tid) wl.init_thread(me, tid);
    else wl.re_init_thread(me);

    using extract_ret_t = std::optional<std::pair<uint32_t,uint32_t>>;
    auto call_extract = [&]() {
        me->op_begin();
        auto ret = wl.extract_min_strict(me);
        me->op_end();
        return ret;
    };

    #if defined(PROFILING) && defined(PROFILING_SERIAL)
    long cnt = 0;
    int print_rate = 20000;
    if (tid > 0) std::cout << "WARNING: PROFILING is defined, but using >1 thread - subsequent call to dump_ht() is serial\n";
    #endif

    while (true) {
        auto ret_term = try_extract<extract_ret_t>(detector, call_extract); // handles termination detection
        if (!ret_term) break; // TERMINATE
        std::tie(gScore, src) = ret_term.value();

        uint64_t targetData = data[targetNode].load(std::memory_order_relaxed);
        uint32_t targetDist = targetData & FSCORE_MASK;
        ++iter;

        #ifdef PROFILING
        if (++cnt % print_rate == 0) {
            std::cout << "(cnt=" << cnt << ")\n";
            wl.dump_ht();
            std::cout << "\n\n";
        }
        #endif

        // With the astar definition, our heuristic
        // will always overestimate. If the current task's
        // gScore is already greater than the targetDist,
        // then it won't ever lead to a shorter path to target.
        if (targetDist <= gScore) {
            ++emptyWork;
            continue;
        }

        uint64_t srcData = data[src].load(std::memory_order_relaxed);
        uint32_t fScore = srcData & FSCORE_MASK;

        for (uint32_t e = 0; e < graph[src].adj.size(); e++) {
            auto& adjNode = graph[src].adj[e];
            uint32_t dst = adjNode.n;
            uint32_t nFScore = fScore + adjNode.d_cm;
            if (targetDist <= nFScore) continue;
            uint64_t dstData = data[dst].load(std::memory_order_relaxed);

            // try CAS the neighbor with the new actual distance
            bool swapped = false;
            do {
                uint32_t dstDist = dstData & FSCORE_MASK;
                if (dstDist <= nFScore) break;
                uint64_t srcShift = src;
                uint64_t swapVal = (srcShift << 32) | nFScore;
                swapped = data[dst].compare_exchange_weak(
                    dstData, swapVal,
                    std::memory_order_acq_rel,
                    std::memory_order_acquire);
            } while(!swapped);
            if (!swapped) continue;

            // compute new heuristic of the neighbor
            uint32_t nGScore = std::max(gScore, nFScore + dist(&graph[dst], &graph[targetNode]));
            if (targetDist <= nGScore) continue;

            // only push if relaxing this vertex is profitable
            me->op_begin();
            wl.insert(me, nGScore, dst);
            me->op_end();
            #ifdef PROFILING
            p_tracker.append_prio(tid, nGScore);
            #endif
            //active_elems++;
        }
    }

    //std::cout << "exiting..." << std::this_thread::get_id() << "\n";

    stats->iter = iter;
    stats->emptyWork = emptyWork;
}

template<typename PQ, typename descriptor>
void MQThreadTaskSTM_Batch(const Vertex* graph, PQ &wl, stat_t *stats,
                    std::atomic<uint64_t> *data, 
                    uint32_t sourceNode, uint32_t targetNode,
                    std::map<int,int> *prios,
                    termination_detector_2& detector, int tid) {
    uint32_t iter = 0UL;
    uint32_t emptyWork = 0UL;
    uint32_t gScore;
    uint32_t src;
    auto* me = new descriptor();
    if (tid) wl.init_thread(me, tid);
    else wl.re_init_thread(me);
    using extract_ret_t = std::optional<std::pair<uint32_t,uint32_t>>;
    auto call_extract = [&]() {
        me->op_begin();
        auto ret = wl.extract_min(me);
        me->op_end();
        return ret;
    };

    while (true) {
        auto ret_term = try_extract<extract_ret_t>(detector, call_extract); // handles termination detection
        if (!ret_term) break; // TERMINATE
        std::tie(gScore, src) = ret_term.value();

        uint64_t targetData = data[targetNode].load(std::memory_order_relaxed);
        uint32_t targetDist = targetData & FSCORE_MASK;
        ++iter;

        // With the astar definition, our heuristic
        // will always overestimate. If the current task's
        // gScore is already greater than the targetDist,
        // then it won't ever lead to a shorter path to target.
        if (targetDist <= gScore) {
            ++emptyWork;
            continue;
        }

        uint64_t srcData = data[src].load(std::memory_order_relaxed);
        uint32_t fScore = srcData & FSCORE_MASK;

        for (uint32_t e = 0; e < graph[src].adj.size(); e++) {
            auto& adjNode = graph[src].adj[e];
            uint32_t dst = adjNode.n;
            uint32_t nFScore = fScore + adjNode.d_cm;
            if (targetDist <= nFScore) continue;
            uint64_t dstData = data[dst].load(std::memory_order_relaxed);

            // try CAS the neighbor with the new actual distance
            bool swapped = false;
            do {
                uint32_t dstDist = dstData & FSCORE_MASK;
                if (dstDist <= nFScore) break;
                uint64_t srcShift = src;
                uint64_t swapVal = (srcShift << 32) | nFScore;
                swapped = data[dst].compare_exchange_weak(
                    dstData, swapVal,
                    std::memory_order_acq_rel,
                    std::memory_order_acquire);
            } while(!swapped);
            if (!swapped) continue;

            // compute new heuristic of the neighbor
            uint32_t nGScore = std::max(gScore, nFScore + dist(&graph[dst], &graph[targetNode]));
            if (targetDist <= nGScore) continue;

            // only push if relaxing this vertex is profitable
            me->op_begin();
            wl.insert_batch(me, nGScore, dst);
            me->op_end();
            //active_elems++;
        }
        me->op_begin();
        wl.flush_batch(me, true);
        me->op_end();

    }

    //std::cout << "exiting..." << std::this_thread::get_id() << "\n";

    stats->iter = iter;
    stats->emptyWork = emptyWork;
}

template <typename MQ_Type>
void spawnTasksLinden(const Vertex* graph, MQ_Type &wl, std::atomic<uint64_t> *data, 
                uint32_t threadNum, uint32_t sourceNode, uint32_t targetNode, termination_detector_2& detector, thread_data_t *data_spray=nullptr) {

    stat_t stats[threadNum];

    auto begin = std::chrono::high_resolution_clock::now();
    std::vector<std::thread*> workers;
    cpu_set_t cpuset;
    for (int i = 1; i < threadNum; i++) {
        CPU_ZERO(&cpuset);
        uint32_t coreID = pinning[i];
        CPU_SET(coreID, &cpuset);
        std::thread *newThread = new std::thread(
            ThreadTaskLinden<MQ_Type>, std::ref(graph), 
            std::ref(wl), &stats[i], std::ref(data),
            sourceNode, targetNode, std::ref(detector)
        );
       
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
    ThreadTaskLinden<MQ_Type>(graph, wl, &stats[0], data, sourceNode, targetNode, std::ref(detector));

    for (std::thread*& worker : workers) {
        worker->join();
        delete worker;
    }

    auto end = std::chrono::high_resolution_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(end-begin).count();
    // if (!wl.empty()) {
    //     std::cout << "not empty\n";
    // }

    //wl.stat_t();

    uint64_t totalIter = 0;
    uint64_t totalEmptyWork = 0;
    for (int i = 0; i < threadNum; i++) {
        totalIter += stats[i].iter;
        totalEmptyWork += stats[i].emptyWork;
    }
    std::cout << "empty work " << totalEmptyWork << "\n";
    std::cout << "runtime_ms " << ms << "\n";
}

template <typename MQ_Type>
void spawnTasksSpray(const Vertex* graph, MQ_Type &wl, std::atomic<uint64_t> *data, 
                uint32_t threadNum, uint32_t sourceNode, uint32_t targetNode, termination_detector_2& detector, thread_data_t *data_spray) {

    stat_t stats[threadNum];

    auto begin = std::chrono::high_resolution_clock::now();
    std::vector<std::thread*> workers;
    cpu_set_t cpuset;
    for (int i = 1; i < threadNum; i++) {
        CPU_ZERO(&cpuset);
        uint32_t coreID = pinning[i];
        CPU_SET(coreID, &cpuset);
        std::thread *newThread = new std::thread(
            ThreadTaskSpray<MQ_Type>, std::ref(graph), 
            std::ref(wl), &stats[i], std::ref(data),
            sourceNode, targetNode, std::ref(detector),
            data_spray, i
        );
       
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
    ThreadTaskSpray<MQ_Type>(graph, wl, &stats[0], data, sourceNode, targetNode, std::ref(detector), data_spray, 0);

    for (std::thread*& worker : workers) {
        worker->join();
        delete worker;
    }

    auto end = std::chrono::high_resolution_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(end-begin).count();

    uint64_t totalIter = 0;
    uint64_t totalEmptyWork = 0;
    for (int i = 0; i < threadNum; i++) {
        totalIter += stats[i].iter;
        totalEmptyWork += stats[i].emptyWork;
    }
    std::cout << "empty work " << totalEmptyWork << "\n";
    std::cout << "runtime_ms " << ms << "\n";
}

template <typename MQ_Type>
void spawnTasks_SMQ(const Vertex* graph, MQ_Type &wl, std::atomic<uint64_t> *data, 
                uint32_t threadNum, uint32_t sourceNode, uint32_t targetNode, termination_detector_2& detector) {

    stat_t stats[threadNum];

    auto begin = std::chrono::high_resolution_clock::now();
    std::vector<std::thread*> workers;
    cpu_set_t cpuset;
    for (int i = 1; i < threadNum; i++) {
        CPU_ZERO(&cpuset);
        uint32_t coreID = pinning[i];
        CPU_SET(coreID, &cpuset);
        std::thread *newThread = new std::thread(
            ThreadTaskSMQ<MQ_Type>, i, std::ref(graph), 
            std::ref(wl), &stats[i], std::ref(data),
            sourceNode, targetNode, std::ref(detector)
        );
        
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

    ThreadTaskSMQ<MQ_Type>(0, graph, wl, &stats[0], data, sourceNode, targetNode, std::ref(detector));

    for (std::thread*& worker : workers) {
        worker->join();
        delete worker;
    }

    auto end = std::chrono::high_resolution_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(end-begin).count();
    // if (!wl.empty()) {
    //     std::cout << "not empty\n";
    // }

    //wl.stat_t();

    uint64_t totalIter = 0;
    uint64_t totalEmptyWork = 0;
    for (int i = 0; i < threadNum; i++) {
        totalIter += stats[i].iter;
        totalEmptyWork += stats[i].emptyWork;
    }
    std::cout << "empty work " << totalEmptyWork << "\n";
    std::cout << "runtime_ms " << ms << "\n";
}

template <typename MQ_Type>
void spawnTasks_PIPQ(const Vertex* graph, MQ_Type &wl, std::atomic<uint64_t> *data, 
                uint32_t threadNum, uint32_t sourceNode, uint32_t targetNode, termination_detector_2& detector) {

    stat_t stats[threadNum];

    auto begin = std::chrono::high_resolution_clock::now();
    std::vector<std::thread*> workers;
    cpu_set_t cpuset;
    for (int i = 1; i < threadNum; i++) {
        CPU_ZERO(&cpuset);
        uint32_t coreID = pinning[i];
        CPU_SET(coreID, &cpuset);
        std::thread *newThread = new std::thread(
            ThreadTaskPIPQ<MQ_Type>, i, std::ref(graph), 
            std::ref(wl), &stats[i], std::ref(data),
            sourceNode, targetNode, std::ref(detector)
        );
        
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

    ThreadTaskPIPQ<MQ_Type>(0, graph, wl, &stats[0], data, sourceNode, targetNode, std::ref(detector));

    for (std::thread*& worker : workers) {
        worker->join();
        delete worker;
    }

    auto end = std::chrono::high_resolution_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(end-begin).count();
    // if (!wl.empty()) {
    //     std::cout << "not empty\n";
    // }

    //wl.stat_t();

    uint64_t totalIter = 0;
    uint64_t totalEmptyWork = 0;
    for (int i = 0; i < threadNum; i++) {
        totalIter += stats[i].iter;
        totalEmptyWork += stats[i].emptyWork;
    }
    std::cout << "empty work " << totalEmptyWork << "\n";
    std::cout << "runtime_ms " << ms << "\n";
}

template <typename MQ_Type, typename descriptor>
void spawnTasksSTM(const Vertex* graph, MQ_Type &wl, std::atomic<uint64_t> *data, 
                uint32_t threadNum, uint32_t sourceNode, uint32_t targetNode,
                bool strict, bool batch, termination_detector_2& detector, prio_tracker& p_tracker) {

    stat_t stats[threadNum];

    std::vector<std::map<int, int>> seen_prios(threadNum);

    if (strict) {
        std::cout << "Running SkiphashPQ STRICT\n";
    } else if (batch) {
        std::cout << "Running SkiphashPQ BATCH-ALL\n";
    } else {
        std::cout << "Running SkiphashPQ BATCH-DEL\n";
    }

    auto begin = std::chrono::high_resolution_clock::now();
    std::vector<std::thread*> workers;
    cpu_set_t cpuset;
    for (int i = 1; i < threadNum; i++) {
        CPU_ZERO(&cpuset);
        uint32_t coreID = pinning[i];
        CPU_SET(coreID, &cpuset);
        std::thread *newThread;

        if (strict) {
            newThread = new std::thread(
                MQThreadTaskSTM_Strict<MQ_Type, descriptor>, std::ref(graph), 
                std::ref(wl), &stats[i], std::ref(data),
                sourceNode, targetNode, &(seen_prios[i]), std::ref(detector), std::ref(p_tracker), i
            );
        } else if (batch) {
            newThread = new std::thread(
                MQThreadTaskSTM_Batch<MQ_Type, descriptor>, std::ref(graph), 
                std::ref(wl), &stats[i], std::ref(data),
                sourceNode, targetNode, &(seen_prios[i]), std::ref(detector), i
            );
        } else {
            newThread = new std::thread(
                MQThreadTaskSTM<MQ_Type, descriptor>, std::ref(graph), 
                std::ref(wl), &stats[i], std::ref(data),
                sourceNode, targetNode, &(seen_prios[i]), std::ref(detector), i
            );
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

    if (strict) MQThreadTaskSTM_Strict<MQ_Type, descriptor>(graph, wl, &stats[0], data, sourceNode, targetNode, &(seen_prios[0]), detector, std::ref(p_tracker), 0);
    else if (batch) MQThreadTaskSTM_Batch<MQ_Type, descriptor>(graph, wl, &stats[0], data, sourceNode, targetNode, &(seen_prios[0]), detector, 0);
    else MQThreadTaskSTM<MQ_Type, descriptor>(graph, wl, &stats[0], data, sourceNode, targetNode, &(seen_prios[0]), detector, 0);
    
    for (std::thread*& worker : workers) {
        worker->join();
        delete worker;
    }

    auto end = std::chrono::high_resolution_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(end-begin).count();
    auto *me = new descriptor();
    if (!wl.empty(me)) {
        std::cout << "not empty\n";
    }

    //wl.stat_t();

    uint64_t totalIter = 0;
    uint64_t totalEmptyWork = 0;
    for (int i = 0; i < threadNum; i++) {
        totalIter += stats[i].iter;
        totalEmptyWork += stats[i].emptyWork;
    }
    std::cout << "empty work " << totalEmptyWork << "\n";
    std::cout << "runtime_ms " << ms << "\n";
}

void astarSTM(Vertex* graph, std::string qType, uint32_t numNodes, 
             uint32_t sourceNode, uint32_t targetNode, 
             uint32_t threadNum, uint32_t chunksize, 
             uint32_t num_chunks, uint32_t sl_max_levels,
             uint32_t buckets, uint32_t delta,
             uint32_t strict, uint32_t batch,
             termination_detector_2& detector)
{
    std::atomic<uint64_t> *data = new std::atomic<uint64_t>[numNodes];
    for (uint i = 0; i < numNodes; i++) {
        data[i].store(UINT64_MAX, std::memory_order_relaxed);
    }
    data[sourceNode] = FSCORE_MASK << 32; // source has no parent

    std::function<void(uint32_t)> prefetcher = [&] (uint32_t v) -> void {
        __builtin_prefetch(&data[v], 0, 3);
    };

    #define _ALG eager_noext_c1_t
    #define _OREC orec_po_t
    #define _CLOCK rdtscp_clock_t

    prio_tracker p_tracker(threadNum, delta);

    using descriptor = _ALG<_OREC, clock_policy::_CLOCK>;
    using map = skiphash_pq_relaxed<uint32_t, uint32_t, _ALG<_OREC, clock_policy::_CLOCK>>;

    config_t cfg;
    cfg.max_levels = sl_max_levels;
    cfg.buckets = buckets;
    cfg.chunksize = chunksize;
    cfg.max_batch_size = chunksize;
    cfg.num_queues = num_chunks;
    cfg.delta = delta;
    std::cout << "cfg.max_levels: " << static_cast<int>(cfg.max_levels) << "\n";
    std::cout << "cfg.buckets: " << cfg.buckets << "\n";
    std::cout << "cfg.chunksize: " << cfg.chunksize << "\n";
    std::cout << "cfg.num_queues: " << cfg.num_queues << "\n";
    std::cout << "cfg.delta: " << cfg.delta << "\n";

    auto *me = new descriptor();
    map pq(me, &cfg);
    pq.init_thread(me, 0);

    me->op_begin();
    pq.insert(me, 0, sourceNode);
    me->op_end();
    #ifdef PROFILING
    p_tracker.append_prio(0, 0);
    #endif

    spawnTasksSTM<map, descriptor>(graph, pq, data, threadNum, sourceNode, targetNode, strict, batch, detector, p_tracker);  

    #ifdef PROFILING
    p_tracker.print_sum_prios();
    #endif

    // trace back the path for verification
    uint32_t cur = targetNode;
    while (cur != sourceNode) {
        uint32_t parent = data[cur].load(std::memory_order_relaxed) >> 32;
        graph[cur].prev = parent;
        cur = parent;
    }

    delete [] data;
}

void astarMQ(Vertex* graph, std::string qType, uint32_t numNodes, 
             uint32_t sourceNode, uint32_t targetNode, 
             uint32_t threadNum, uint32_t queueNum, 
             uint32_t batchSizePop, uint32_t batchSizePush,
             uint32_t delta, uint32_t bucketNum)
{
    std::atomic<uint64_t> *data = new std::atomic<uint64_t>[numNodes];
    for (uint i = 0; i < numNodes; i++) {
        data[i].store(UINT64_MAX, std::memory_order_relaxed);
    }
    data[sourceNode] = FSCORE_MASK << 32; // source has no parent

    std::function<void(uint32_t)> prefetcher = [&] (uint32_t v) -> void {
        __builtin_prefetch(&data[v], 0, 3);
    };

    //!!!!!!
    
    // if (qType == "MQBucket") {
    //     printf("delta: %d\n", delta);
    //     printf("Buckets: %d\n", bucketNum);
    //     std::function<mbq::BucketID(uint32_t)> getBucketID = [&] (uint32_t v) -> mbq::BucketID {
    //         uint64_t d = data[v].load(std::memory_order_acquire);
    //         uint32_t fScore = d & FSCORE_MASK;
    //         uint32_t gScore = fScore + dist(&graph[v], &graph[targetNode]);
    //         return mbq::BucketID(gScore) >> delta;
    //     };
    //     using MQ_Bucket = mbq::MultiBucketQueue<
    //         decltype(getBucketID), decltype(prefetcher), 
    //         std::greater<mbq::BucketID>, uint32_t, uint32_t, true, true
    //     >;
    //     MQ_Bucket wl(getBucketID, prefetcher, queueNum, threadNum, delta, 
    //                  bucketNum, batchSizePop, batchSizePush, mbq::increasing);
    //     wl.push(0, sourceNode);
    //     spawnTasks<MQ_Bucket>(graph, wl, data, threadNum, sourceNode, targetNode);  

    // } else if (qType == "MQ") {
    //     // qType == MQ
    //     using MQ = mbq::MultiQueue<decltype(prefetcher), std::greater<PQElement>, uint32_t, uint32_t>;
    //     MQ wl(prefetcher, queueNum, threadNum, batchSizePop, batchSizePush);
    //     wl.push(0, sourceNode);
    //     spawnTasks<MQ>(graph, wl, data, threadNum, sourceNode, targetNode);  
    // }

    // trace back the path for verification
    uint32_t cur = targetNode;
    while (cur != sourceNode) {
        uint32_t parent = data[cur].load(std::memory_order_relaxed) >> 32;
        graph[cur].prev = parent;
        cur = parent;
    }

    delete [] data;
}

void astarCompetitors(Vertex* graph, std::string qType, uint32_t numNodes, 
             uint32_t sourceNode, uint32_t targetNode, 
             uint32_t threadNum, termination_detector_2& detector)
{
    std::atomic<uint64_t> *data = new std::atomic<uint64_t>[numNodes];
    for (uint i = 0; i < numNodes; i++) {
        data[i].store(UINT64_MAX, std::memory_order_relaxed);
    }
    data[sourceNode] = FSCORE_MASK << 32; // source has no parent

    std::function<void(uint32_t)> prefetcher = [&] (uint32_t v) -> void {
        __builtin_prefetch(&data[v], 0, 3);
    };
    
    if (qType == "SMQ") {
        const size_t steal_probability = 8; // 1/8 probability of stealing
        const size_t steal_batch_size = 128; // size of batch to steal

        using smq_t = smq_ns::StealingMultiQueue<std::pair<uint32_t,uint32_t>,uint32_t,steal_probability,steal_batch_size,true>;
        auto smq_ds = smq_t(threadNum);
        smq_ds.init_thread(0);
        smq_ds.insert(0, sourceNode);
        spawnTasks_SMQ<smq_t>(graph, smq_ds, data, threadNum, sourceNode, targetNode, detector);  
    } else if (qType == "PIPQ") {
        int heap_list_size = 5000000;
        int cntr_tsh = 10;
        int cntr_max = 100;

        using pipq_t = pq_ns::pipq<uint32_t>;

        auto numa_pq_ds = pipq_t(heap_list_size, threadNum, cntr_tsh, cntr_max, pinning);
        numa_pq_ds.PQInit();
        numa_pq_ds.init_thread(0);
        numa_pq_ds.push(0, sourceNode);
        spawnTasks_PIPQ<pipq_t>(graph, numa_pq_ds, data, threadNum, sourceNode, targetNode, detector);
    } else if (qType == "Linden") {
        int max_offset = 32;
        _init_gc_subsystem();
        pq_t * linden_pq = pq_init(max_offset);
        insert(linden_pq, 0, sourceNode);
        spawnTasksLinden<pq_t*>(graph, linden_pq, data, threadNum, sourceNode, targetNode, detector);
    } else if (qType == "Spraylist") {
        *levelmax = 32;
        thread_data_t *data_spray = (thread_data_t *)malloc(threadNum * sizeof(thread_data_t));
        for (int i = 0; i < threadNum; i++) {
            data_spray[i].seed = rand();
            data_spray[i].seed2 = rand();
            data_spray[i].nb_threads = threadNum;
        }
        //ssalloc_init();
        seeds = seed_rand();
        std::cout << "Calling sl_set_new()\n";
        sl_intset_t * sl_spray = sl_set_new(); //sl_add_val(set, 0, src, 0);
        std::cout << "Created set\n";
        fraser_insert(sl_spray, 0, sourceNode);
        spawnTasksSpray<sl_intset_t *>(graph, sl_spray, data, threadNum, sourceNode, targetNode, detector, data_spray);
    } else {
        std::cout << "ERROR: invalid data structure\n";
        std::terminate();
    }

    // trace back the path for verification
    uint32_t cur = targetNode;
    while (cur != sourceNode) {
        uint32_t parent = data[cur].load(std::memory_order_relaxed) >> 32;
        graph[cur].prev = parent;
        cur = parent;
    }

    delete [] data;
}

void astarSerial(Vertex* graph, uint32_t numNodes, 
                uint32_t sourceNode, uint32_t targetNode)
{
    using PQ = std::priority_queue<
        PQElement, 
        std::vector<PQElement>,
        std::greater<PQElement>
    >;
    PQ wl;

    std::vector<uint32_t> queuesizes;

    std::vector<uint32_t> prios(numNodes, UINT32_MAX);
    prios[sourceNode] = 0;
    wl.push(std::make_tuple(0, sourceNode));

    auto begin = std::chrono::high_resolution_clock::now();

    uint32_t gScore;
    uint32_t src;
    uint32_t iter = 0;
    uint32_t emptyWork = 0;
    uint32_t maxSize = 0;
    while (!wl.empty()) {
        if (wl.size() > maxSize) maxSize = wl.size();
        queuesizes.push_back(wl.size());
        std::tie(gScore, src) = wl.top();
        wl.pop();

        uint32_t targetDist = prios[targetNode];
        uint32_t fScore = prios[src];
        ++iter;

        // With the astar definition, our heuristic
        // will always overestimate. If the current task's
        // gScore is already greater than the targetDist,
        // then it won't ever lead to a shorter path to target.
        if (targetDist <= gScore) {
            ++emptyWork;
            continue;
        }

        for (uint32_t e = 0; e < graph[src].adj.size(); e++) {
            auto& adjNode = graph[src].adj[e];
            uint32_t dst = adjNode.n;
            uint32_t nFScore = fScore + adjNode.d_cm;
            if (targetDist <= nFScore) continue;
            uint32_t d = prios[dst];
            if (d <= nFScore) continue;

            // compute new heuristic of the neighbor
            uint32_t nGScore = std::max(gScore, nFScore + dist(&graph[dst], &graph[targetNode]));
            if (targetDist <= nGScore) continue;

            // only push if relaxing this vertex is profitable
            prios[dst] = nFScore;
            graph[dst].prev = src;
            wl.push({nGScore, dst});
        }
    }


    auto end = std::chrono::high_resolution_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(end-begin).count();
    std::cout << "runtime_ms " << ms << "\n";
    std::cout << "iter " << iter << "\n";
    std::cout << "empty work " << emptyWork << "\n";
    std::cout << "max size " << maxSize << "\n";

    uint64_t sum = 0;
    for (uint i = 0; i < queuesizes.size(); i++) {
        sum += queuesizes[i];
    }
    double s = sum;
    double n = queuesizes.size();
    double avg = s / n;
    std::cout << "avg size = " << avg << "\n";
}

int main(int argc, const char** argv) {
    if (argc < 2) {
        printf("Types: Serial / MQ / MQBucket\n");
        printf("Usage: %s <inFile> <startNode> <endNode> ", argv[0]);
        printf("[qType threadNum queueNum batchPop batchPush delta bucketNum printFull]\n\n");
        
        printf("Types: SkipHashPQ\n");
        printf("Usage: %s <inFile> <startNode> <endNode> ", argv[0]);
        printf("[qType threadNum chunksize num_chunks strict delta num_buckets sl_max_levels ]\n");
        
        return -1;
    }

    Vertex* graph;
    uint32_t numNodes;
    std::tie(graph, numNodes) = LoadGraph(argv[1]);

    uint32_t sourceNode = (argc > 2) ? std::min((uint32_t)atoi(argv[2]), numNodes-1) : 1*numNodes/10;
    uint32_t targetNode = (argc > 3) ? std::min((uint32_t)atoi(argv[3]), numNodes-1) : 9*numNodes/10;
    std::string qType = (argc > 4) ? argv[4] : "Serial";
    uint32_t threadNum = (argc > 5) ? atol(argv[5]) : 1;
    // for MQ / Bucket:
    uint32_t queueNum, batchSizePop, batchSizePush, delta, bucketNum, printFull;
    // for SkipHashPQ:
    uint32_t chunksize, num_chunks, sl_max_levels, buckets, strict, batch;

    if (qType == "SkipHashPQ") {
        chunksize = (argc > 6) ? atol(argv[6]) : 128;
        num_chunks = (argc > 7) ? atol(argv[7]) : 8;
        strict = (argc > 8) ? atol(argv[8]) : 0;
        batch = (argc > 9) ? atol(argv[9]) : 0;
        delta = (argc > 10) ? atol(argv[10]) : 0;
        buckets = (argc > 11) ? atol(argv[11]) : 1048576;
        sl_max_levels = (argc > 12) ? atol(argv[12]) : 32;
    } else {
        queueNum = (argc > 6) ? atol(argv[6]) : threadNum * 4;
        batchSizePop = (argc > 7) ? atol(argv[7]) : 1;
        batchSizePush = (argc > 8) ? atol(argv[8]) : 1;
        delta = (argc > 9) ? atol(argv[9]) : 10;
        bucketNum = (argc > 10) ? atol(argv[10]) : 64;
        printFull = (argc > 11) ? atol(argv[11]) : 0;
    }
    
    printf("Finding shortest path between nodes %d and %d\n", sourceNode, targetNode);
    printf("Type: %s\n", qType.c_str());
    printf("Threads: %d\n", threadNum);

    termination_detector_2 detector_2(threadNum);

    if (qType == "Serial") {
        astarSerial(graph, numNodes, sourceNode, targetNode);
    } else if (qType == "MQ" || qType == "MQBucket") {
        printf("Queues: %d\n", queueNum);
        printf("batchSizePop: %d\n", batchSizePop);
        printf("batchSizePush: %d\n\n", batchSizePush);

        astarMQ(graph, qType, numNodes, sourceNode, targetNode, threadNum, queueNum,
                batchSizePop, batchSizePush, delta, bucketNum);
    } else if (qType == "SkipHashPQ") {
        printf("Chunk size: %d\n", chunksize);
        printf("Num chunks: %d\n", num_chunks);
        printf("Skiplist max levels: %d\n", sl_max_levels);
        printf("Num buckets: %d\n", buckets);

        astarSTM(graph, qType, numNodes, sourceNode, targetNode, threadNum,
                chunksize, num_chunks, sl_max_levels, buckets, delta, strict, batch, detector_2);
    } else if (qType == "SMQ" || qType == "PIPQ" || qType == "Linden" || qType == "Spraylist") {
        astarCompetitors(graph, qType, numNodes, sourceNode, targetNode, threadNum, detector_2);
    } else {
        std::cerr << "Unrecognized type: " << qType << "\n";
        return 1;
    }

    // Print the resulting path
    std::vector<uint32_t> path;
    uint32_t cur = targetNode;
    while (true) {
        path.push_back(cur);
        if (cur == sourceNode) break;
        cur = graph[cur].prev;
        // assert(cur);
    }
    std::reverse(path.begin(), path.end());

    uint32_t totalDist_cm = 0;
    for (uint32_t i = 0; i < path.size()-1; i++) {
        uint32_t curDist_cm = neighDist(graph, path[i], path[i+1]);
        totalDist_cm += curDist_cm;
        if (printFull)
            printf("%4d: %9d -> %9d | %8d.%02d m | %8d.%02d m\n", i, path[i], path[i+1],
                    curDist_cm / 100, curDist_cm % 100, totalDist_cm / 100, totalDist_cm % 100);
    }
    printf("total distance: %8d.%02d m\n", totalDist_cm / 100, totalDist_cm % 100);
            
    uint32_t directDist_cm = dist(&graph[sourceNode], &graph[targetNode]);
    printf("As-the-crow-flies distance: %8d.%02d m\n", directDist_cm / 100, directDist_cm % 100);

    // Save the path coordinates in binary
    // FILE* outFile = fopen("path.bin", "wb");
    // for (uint32_t vID : path) {
    //     Vertex* v = &graph[vID];
    //     fwrite(&v->lat, sizeof(double), 1, outFile);
    //     fwrite(&v->lon, sizeof(double), 1, outFile);
    // }
    // fclose(outFile);

    // Save the path in txt
    std::ofstream outFile("path.txt");
    outFile << "start: " << sourceNode << "\n";
    outFile << "target: " << targetNode << "\n";
    outFile << "total distance: " << totalDist_cm / 100 << "." << totalDist_cm % 100 << "m\n";
    outFile << "As-the-crow-flies distance: " << directDist_cm / 100 << "." << directDist_cm % 100 << "m\n";
    for (uint32_t vID : path) {
        outFile << vID << "\n";
    }
    outFile.close();


    return 0;
}
