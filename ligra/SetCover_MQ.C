// [mcj] Use the AdjacencyGraph format (e.g. in inputs/pbbs/mis/*)

#include "ligra.h"
#include "SetCover.h"
#include "MultiQueue.h"
#include "MultiBucketQueue.h"

#include <cassert>
#include <cstdio>
#include <thread>
#include <limits>
#include <vector>
#include <functional>
#include <set>

#include "../galois/lonestar/analytics/cpu/termination_helper.h"
#include "../galois/lonestar/analytics/cpu/config.h"

#include <optstm2/eager_noext_c1.h>
#include "skiphash_pq_relaxed.h"

#include "pipq_impl.h"
#include "smq_impl.h"
#include "linden.h"
#include "gc/ptst.h"

#include "intset.h"
#include "random.h"

#include "../priority_tracker.h"

OPTSTM2_GLOBALS_INITIALIZER;

const int pinning[96] = {0,2,4,6,8,10,12,14,16,18,20,22,24,26,28,30,32,34,36,38,40,42,44,46,1,3,5,7,9,11,13,15,17,19,21,23,25,27,29,31,33,35,37,39,41,43,45,47,48,50,52,54,56,58,60,62,64,66,68,70,72,74,76,78,80,82,84,86,88,90,92,94,49,51,53,55,57,59,61,63,65,67,69,71,73,75,77,79,81,83,85,87,89,91,93,95};

alignas(64) extern uint8_t levelmax[64];
__thread unsigned long *seeds;

std::vector<std::unordered_map<int, int>> prios(96);

// alignas(64) extern uint8_t levelmax[64];
// __thread unsigned long *seeds;

using PQElement = std::tuple<uintE, uintE>;

struct stats {
  uint64_t emptyWork = 0;
};

template <class vertex, typename MQ>
void MQThreadTask(graph<vertex>& G, MQ& wl,
                        atomic_flag* isElemCovered,
                        atomic<uint32_t>* cardinality,
                        atomic_flag* cover,
                        stats* threadStat,
                        termination_detector_2& detector)
{
    uint64_t emptyWork = 0;
    wl.initTID();
    using extract_ret_t = boost::optional<tuple<uintE,uintE>>;
    auto call_extract = [&]() {
        return wl.popInternal();
    };

    while (true) {
        // Pop the max-cardinality (degree) set (vertex)
        uintE pushedCard, s;
        auto ret_term = try_extract<extract_ret_t>(detector, call_extract); // handles termination detection
        if (!ret_term) break; // TERMINATE
        std::tie(pushedCard, s) = ret_term.value();

        if (pushedCard == 0) {
            emptyWork++;
            continue;
        }

        // postponed and all its elements are already covered
        // should not be added to subcover
        uint32_t curCard = cardinality[s].load(std::memory_order_acquire);
        if (curCard == 0) {
            emptyWork++;
            continue;
        }

        // postponed
        if (pushedCard > curCard) {
            wl.push(curCard, s);
            emptyWork++;
            continue;
        }

        // check if this is already in subcover
        if (cover[s].test() || cover[s].test_and_set()) {
            emptyWork++;
            continue;
        }

        // Delete Set v's member Elements from other Sets
        const vertex& vs = G.V[s];
        size_t sD = vs.getOutDegree();
        for (size_t i = 0; i < sD; i++) {
            uintE elem = vs.getOutNeighbor(i);

            // check if this node's member elements
            // have already been processed
            if (isElemCovered[elem].test() || isElemCovered[elem].test_and_set())
                continue;

            const vertex& ve = G.V[elem];
            size_t elemD = ve.getInDegree();
            for (size_t j = 0; j < elemD; j++) {
                uintE s1 = ve.getInNeighbor(j);
                if (s1 == s) continue;

                // decrease the outCardinlaity (priority)
                uint32_t card = cardinality[s1].load(std::memory_order_relaxed);
                bool decreased = false;
                while (!decreased && card > 0) {
                    decreased = cardinality[s1].compare_exchange_weak(
                        card, card - 1,
                        memory_order_acq_rel, memory_order_acquire);
                };
            }
        }
    }

    threadStat->emptyWork = emptyWork;
}

template <class vertex, typename MQ>
void MQThreadTaskSMQ(graph<vertex>& G, MQ& wl,
                        atomic_flag* isElemCovered,
                        atomic<uint32_t>* cardinality,
                        atomic_flag* cover,
                        stats* threadStat,
                        termination_detector_2& detector,
                        int tid)
{
    uint64_t emptyWork = 0;
    if (tid) wl.init_thread(tid);
    using extract_ret_t = std::optional<std::pair<uintE,uintE>>;
    auto call_extract = [&]() {
        return wl.extract_min();
    };

    while (true) {
        // Pop the max-cardinality (degree) set (vertex)
        uintE pushedCard, s;
        auto ret_term = try_extract<extract_ret_t>(detector, call_extract); // handles termination detection
        if (!ret_term) break; // TERMINATE
        std::tie(pushedCard, s) = ret_term.value();

        if (pushedCard == 0) {
            emptyWork++;
            continue;
        }

        // postponed and all its elements are already covered
        // should not be added to subcover
        uint32_t curCard = cardinality[s].load(std::memory_order_acquire);
        if (curCard == 0) {
            emptyWork++;
            continue;
        }

        // postponed
        if (pushedCard > curCard) {
            wl.insert(curCard, s);
            emptyWork++;
            continue;
        }

        // check if this is already in subcover
        if (cover[s].test() || cover[s].test_and_set()) {
            emptyWork++;
            continue;
        }

        // Delete Set v's member Elements from other Sets
        const vertex& vs = G.V[s];
        size_t sD = vs.getOutDegree();
        for (size_t i = 0; i < sD; i++) {
            uintE elem = vs.getOutNeighbor(i);

            // check if this node's member elements
            // have already been processed
            if (isElemCovered[elem].test() || isElemCovered[elem].test_and_set())
                continue;

            const vertex& ve = G.V[elem];
            size_t elemD = ve.getInDegree();
            for (size_t j = 0; j < elemD; j++) {
                uintE s1 = ve.getInNeighbor(j);
                if (s1 == s) continue;

                // decrease the outCardinlaity (priority)
                uint32_t card = cardinality[s1].load(std::memory_order_relaxed);
                bool decreased = false;
                while (!decreased && card > 0) {
                    decreased = cardinality[s1].compare_exchange_weak(
                        card, card - 1,
                        memory_order_acq_rel, memory_order_acquire);
                };
            }
        }
    }

    threadStat->emptyWork = emptyWork;
}

template <class vertex, typename MQ>
void MQThreadTaskLinden(graph<vertex>& G, MQ& wl,
                        atomic_flag* isElemCovered,
                        atomic<uint32_t>* cardinality,
                        atomic_flag* cover,
                        stats* threadStat,
                        termination_detector_2& detector,
                        int tid)
{
    uint64_t emptyWork = 0;
    using extract_ret_t = std::optional<std::pair<uintE,uintE>>;
    auto call_extract = [&]() {
        return extract_min(wl);
    };

    while (true) {
        // Pop the max-cardinality (degree) set (vertex)
        uintE pushedCard, s;
        auto ret_term = try_extract<extract_ret_t>(detector, call_extract); // handles termination detection
        if (!ret_term) break; // TERMINATE
        std::tie(pushedCard, s) = ret_term.value();

        if (pushedCard == 0) {
            emptyWork++;
            continue;
        }

        // postponed and all its elements are already covered
        // should not be added to subcover
        uint32_t curCard = cardinality[s].load(std::memory_order_acquire);
        if (curCard == 0) {
            emptyWork++;
            continue;
        }

        // postponed
        if (pushedCard > curCard) {
            insert(wl, curCard, s);
            emptyWork++;
            continue;
        }

        // check if this is already in subcover
        if (cover[s].test() || cover[s].test_and_set()) {
            emptyWork++;
            continue;
        }

        // Delete Set v's member Elements from other Sets
        const vertex& vs = G.V[s];
        size_t sD = vs.getOutDegree();
        for (size_t i = 0; i < sD; i++) {
            uintE elem = vs.getOutNeighbor(i);

            // check if this node's member elements
            // have already been processed
            if (isElemCovered[elem].test() || isElemCovered[elem].test_and_set())
                continue;

            const vertex& ve = G.V[elem];
            size_t elemD = ve.getInDegree();
            for (size_t j = 0; j < elemD; j++) {
                uintE s1 = ve.getInNeighbor(j);
                if (s1 == s) continue;

                // decrease the outCardinlaity (priority)
                uint32_t card = cardinality[s1].load(std::memory_order_relaxed);
                bool decreased = false;
                while (!decreased && card > 0) {
                    decreased = cardinality[s1].compare_exchange_weak(
                        card, card - 1,
                        memory_order_acq_rel, memory_order_acquire);
                };
            }
        }
    }

    threadStat->emptyWork = emptyWork;
}

template <class vertex, typename MQ>
void MQThreadTaskSpray(graph<vertex>& G, MQ& wl,
                        atomic_flag* isElemCovered,
                        atomic<uint32_t>* cardinality,
                        atomic_flag* cover,
                        stats* threadStat,
                        termination_detector_2& detector,
                        int tid, thread_data_t *data)
{
    uint64_t emptyWork = 0;
    using extract_ret_t = std::optional<std::pair<uintE,uintE>>;
    thread_data_t* data_item = &(data[tid]);

    auto call_extract = [&]() {
        return spray_delete_min_key(wl, data_item);
    };

    if (tid) {
        seeds = seed_rand();
    }

    while (true) {
        // Pop the max-cardinality (degree) set (vertex)
        uintE pushedCard, s;
        auto ret_term = try_extract<extract_ret_t>(detector, call_extract); // handles termination detection
        if (!ret_term) break; // TERMINATE
        std::tie(pushedCard, s) = ret_term.value();

        if (pushedCard == 0) {
            emptyWork++;
            continue;
        }

        // postponed and all its elements are already covered
        // should not be added to subcover
        uint32_t curCard = cardinality[s].load(std::memory_order_acquire);
        if (curCard == 0) {
            emptyWork++;
            continue;
        }

        // postponed
        if (pushedCard > curCard) {
            fraser_insert(wl, curCard, s);
            emptyWork++;
            continue;
        }

        // check if this is already in subcover
        if (cover[s].test() || cover[s].test_and_set()) {
            emptyWork++;
            continue;
        }

        // Delete Set v's member Elements from other Sets
        const vertex& vs = G.V[s];
        size_t sD = vs.getOutDegree();
        for (size_t i = 0; i < sD; i++) {
            uintE elem = vs.getOutNeighbor(i);

            // check if this node's member elements
            // have already been processed
            if (isElemCovered[elem].test() || isElemCovered[elem].test_and_set())
                continue;

            const vertex& ve = G.V[elem];
            size_t elemD = ve.getInDegree();
            for (size_t j = 0; j < elemD; j++) {
                uintE s1 = ve.getInNeighbor(j);
                if (s1 == s) continue;

                // decrease the outCardinlaity (priority)
                uint32_t card = cardinality[s1].load(std::memory_order_relaxed);
                bool decreased = false;
                while (!decreased && card > 0) {
                    decreased = cardinality[s1].compare_exchange_weak(
                        card, card - 1,
                        memory_order_acq_rel, memory_order_acquire);
                };
            }
        }
    }

    threadStat->emptyWork = emptyWork;
}

template <class vertex, typename MQ, typename descriptor>
void MQThreadTaskSTM_Strict(graph<vertex>& G, MQ& wl,
                            atomic_flag* isElemCovered,
                            atomic<uint32_t>* cardinality,
                            atomic_flag* cover,
                            stats* threadStat,
                            termination_detector_2& detector,
                            int tid,
                            prio_tracker& p_tracker)
{
    uint64_t emptyWork = 0;
    auto* me = new descriptor();
    using extract_ret_t = std::optional<std::pair<uintE,uintE>>;
    auto call_extract = [&]() {
        me->op_begin();
        auto ret = wl.extract_min_strict(me);
        me->op_end();
        return ret;
    };
    wl.init_thread(me, tid);

    #if defined(PROFILING)
    long cnt = 0;
    int print_rate = 250000;
    if (tid > 0) std::cout << "WARNING: PROFILING is defined, but using >1 thread - subsequent call to dump_ht() is serial\n";
    #endif

    while (true) {
        // Pop the max-cardinality (degree) set (vertex)
        uintE pushedCard, s;

        auto ret_term = try_extract<extract_ret_t>(detector, call_extract); // handles termination detection
        if (!ret_term) break; // TERMINATE
        std::tie(pushedCard, s) = ret_term.value();

        //std::cout << "removed " << pushedCard << "\n";

        #ifdef PROFILING
        if (++cnt % print_rate == 0) {
            std::cout << "(cnt=" << cnt << ")\n";
            wl.dump_ht();
            std::cout << "\n\n";
        }
        #endif

        if (pushedCard == 0) {
            emptyWork++;
            continue;
        }

        // postponed and all its elements are already covered
        // should not be added to subcover
        uint32_t curCard = cardinality[s].load(std::memory_order_acquire);
        if (curCard == 0) {
            emptyWork++;
            continue;
        }

        // postponed
        if (pushedCard > curCard) {
            me->op_begin();
            wl.insert(me, curCard, s);
            me->op_end();
            #ifdef PROFILING
            p_tracker.append_prio(tid, curCard);
            #endif
            //std::cout << "inserted " << curCard << "\n";

            emptyWork++;
            continue;
        }

        // check if this is already in subcover
        if (cover[s].test() || cover[s].test_and_set()) {
            emptyWork++;
            continue;
        }

        // Delete Set v's member Elements from other Sets
        const vertex& vs = G.V[s];
        size_t sD = vs.getOutDegree();
        for (size_t i = 0; i < sD; i++) {
            uintE elem = vs.getOutNeighbor(i);

            // check if this node's member elements
            // have already been processed
            if (isElemCovered[elem].test() || isElemCovered[elem].test_and_set())
                continue;

            const vertex& ve = G.V[elem];
            size_t elemD = ve.getInDegree();
            for (size_t j = 0; j < elemD; j++) {
                uintE s1 = ve.getInNeighbor(j);
                if (s1 == s) continue;

                // decrease the outCardinlaity (priority)
                uint32_t card = cardinality[s1].load(std::memory_order_relaxed);
                bool decreased = false;
                while (!decreased && card > 0) {
                    decreased = cardinality[s1].compare_exchange_weak(
                        card, card - 1,
                        memory_order_acq_rel, memory_order_acquire);
                };
            }
        }
    }

    threadStat->emptyWork = emptyWork;
}

template <class vertex, typename MQ, typename descriptor>
void MQThreadTaskSTM_Batch(graph<vertex>& G, MQ& wl,
                            atomic_flag* isElemCovered,
                            atomic<uint32_t>* cardinality,
                            atomic_flag* cover,
                            stats* threadStat,
                            termination_detector_2& detector, int tid)
{

    uint64_t emptyWork = 0;
    auto* me = new descriptor();
    using extract_ret_t = std::optional<std::pair<uintE,uintE>>;
    auto call_extract = [&]() {
        me->op_begin();
        auto ret = wl.extract_min(me); 
        me->op_end();
        return ret;
    };
    wl.init_thread(me, tid);

    while (true) {
        // Pop the max-cardinality (degree) set (vertex)
        uintE pushedCard, s;

        auto ret_term = try_extract<extract_ret_t>(detector, call_extract); // handles termination detection
        if (!ret_term) break; // TERMINATE
        std::tie(pushedCard, s) = ret_term.value();

        if (pushedCard == 0) {
            emptyWork++;
            continue;
        }

        // postponed and all its elements are already covered
        // should not be added to subcover
        uint32_t curCard = cardinality[s].load(std::memory_order_acquire);
        if (curCard == 0) {
            emptyWork++;
            continue;
        }

        // postponed
        if (pushedCard > curCard) {
            me->op_begin();
            //std::cout << "INSERTED-* " << curCard << "\n";
            wl.insert(me, curCard, s);
            me->op_end();

            emptyWork++;
            continue;
        }

        // check if this is already in subcover
        if (cover[s].test() || cover[s].test_and_set()) {
            emptyWork++;
            continue;
        }

        // Delete Set v's member Elements from other Sets
        const vertex& vs = G.V[s];
        size_t sD = vs.getOutDegree();
        for (size_t i = 0; i < sD; i++) {
            uintE elem = vs.getOutNeighbor(i);

            // check if this node's member elements
            // have already been processed
            if (isElemCovered[elem].test() || isElemCovered[elem].test_and_set())
                continue;

            const vertex& ve = G.V[elem];
            size_t elemD = ve.getInDegree();
            for (size_t j = 0; j < elemD; j++) {
                uintE s1 = ve.getInNeighbor(j);
                if (s1 == s) continue;

                // decrease the outCardinlaity (priority)
                uint32_t card = cardinality[s1].load(std::memory_order_relaxed);
                bool decreased = false;
                while (!decreased && card > 0) {
                    decreased = cardinality[s1].compare_exchange_weak(
                        card, card - 1,
                        memory_order_acq_rel, memory_order_acquire);
                };
            }
        }
    }

    threadStat->emptyWork = emptyWork;
}

template <class vertex, typename MQ_Type>
void spawnTasks(graph<vertex>& G, MQ_Type &wl, int threadNum,
                atomic_flag* isElemCovered, atomic<uint32_t>* cardinality,
                atomic_flag* cover, termination_detector_2& detector, bool noverify=false)
{
    int cnt1 = 0, cnt2 = 0;
    // Queue each Set, prioritized by its current (initial) degree/cardinality
    for (size_t s = 0; s < G.n; s++) {
        uintE outCardinality = G.V[s].getOutDegree();
        uintE inCardinality = G.V[s].getInDegree();

        if (outCardinality > 0) {
            wl.push(outCardinality, s);
            cardinality[s] = outCardinality;
        }

        if (outCardinality == 0) cnt1++;
        if (inCardinality == 0) cnt2++;
    }

    cout << cnt1 << " vertices with no outgoing edge\n";
    cout << cnt2 << " vertices with no incoming edge\n";

    stats threadStats[threadNum];
    for (int i = 0; i < threadNum;i++) {
        threadStats[i].emptyWork = 0;
    }

    auto begin = std::chrono::high_resolution_clock::now();

    vector<thread*> workers;
    cpu_set_t cpuset;
    for (int i = 1; i < threadNum; i++) {
        CPU_ZERO(&cpuset);
        uint64_t coreID = i;
        CPU_SET(coreID, &cpuset);
        std::thread *newThread = new std::thread(
            MQThreadTask<vertex, MQ_Type>, ref(G),
            ref(wl), ref(isElemCovered),
            ref(cardinality), ref(cover),
            &threadStats[i], ref(detector)
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
    MQThreadTask<vertex, MQ_Type>(G, wl, isElemCovered, cardinality, cover, &threadStats[0], detector);
    for (thread*& worker : workers) {
        worker->join();
        delete worker;
    }

    auto end = std::chrono::high_resolution_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(end-begin).count();
    wl.stat();
    std::cout << "runtime_ms " << ms << "\n";

    uint64_t totalEmptyWork = 0;
    for (int i = 0; i < threadNum; i++) {
        totalEmptyWork += threadStats[i].emptyWork;
    }
    cout << "total empty work: " << totalEmptyWork << endl;

    // process cover
    if (!noverify) {
        vector<uintE> checkCover;
        checkCover.reserve(G.n);
        for (int i = 0; i < G.n; i++) {
            if (cover[i].test())
                checkCover.push_back(i);
        }
        if (!setcover::success<vertex>(G, checkCover)) std::cout << "Error occurred, but not aborting\n"; //abort();
    }
}

template <class vertex, typename MQ_Type, typename descriptor>
void spawnTasksSTM(graph<vertex>& G, MQ_Type &wl, int threadNum,
                atomic_flag* isElemCovered, atomic<uint32_t>* cardinality,
                atomic_flag* cover, termination_detector_2& detector, bool strict, bool batch, prio_tracker& p_tracker, bool noverify=false)
{
    int cnt1 = 0, cnt2 = 0;
    auto* me = new descriptor();
    // Queue each Set, prioritized by its current (initial) degree/cardinality
    for (size_t s = 0; s < G.n; s++) {
        uintE outCardinality = G.V[s].getOutDegree();
        uintE inCardinality = G.V[s].getInDegree();

        if (outCardinality > 0) {
            me->op_begin();
            wl.insert(me, outCardinality, s);
            me->op_end();
            cardinality[s] = outCardinality;
            //std::cout << "Inserted " << outCardinality << "\n";
            #ifdef PROFILING
            p_tracker.append_prio(0, outCardinality);
            #endif
        }

        if (outCardinality == 0) cnt1++;
        if (inCardinality == 0) cnt2++;
    }

    cout << cnt1 << " vertices with no outgoing edge\n";
    cout << cnt2 << " vertices with no incoming edge\n";

    stats threadStats[threadNum];
    for (int i = 0; i < threadNum;i++) {
        threadStats[i].emptyWork = 0;
    }

    auto begin = std::chrono::high_resolution_clock::now();

    vector<thread*> workers;
    cpu_set_t cpuset;
    for (int i = 1; i < threadNum; i++) {
        CPU_ZERO(&cpuset);
        uint64_t coreID = i;
        CPU_SET(coreID, &cpuset);

        std::thread *newThread;

        if (strict) {
            newThread = new std::thread(
                MQThreadTaskSTM_Strict<vertex, MQ_Type, descriptor>, ref(G),
                ref(wl), ref(isElemCovered),
                ref(cardinality), ref(cover),
                &threadStats[i], ref(detector), i, std::ref(p_tracker)
            );
        } else if (batch) {
            // batch ins AND batch delmin
            newThread = new std::thread(
                MQThreadTaskSTM_Batch<vertex, MQ_Type, descriptor>, ref(G),
                ref(wl), ref(isElemCovered),
                ref(cardinality), ref(cover),
                &threadStats[i], ref(detector), i
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
    if (strict) MQThreadTaskSTM_Strict<vertex, MQ_Type, descriptor>(G, wl, isElemCovered, cardinality, cover, &threadStats[0], detector, 0, p_tracker);
    else if (batch) MQThreadTaskSTM_Batch<vertex, MQ_Type, descriptor>(ref(G), ref(wl), ref(isElemCovered), ref(cardinality), ref(cover), &threadStats[0], ref(detector), 0);
    for (thread*& worker : workers) {
        worker->join();
        delete worker;
    }

    auto end = std::chrono::high_resolution_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(end-begin).count();
    //wl.stat();

    #ifdef PROFILING
    p_tracker.print_sum_prios();
    #endif
    std::cout << "runtime_ms " << ms << "\n";

    uint64_t totalEmptyWork = 0;
    for (int i = 0; i < threadNum; i++) {
        totalEmptyWork += threadStats[i].emptyWork;
    }
    cout << "total empty work: " << totalEmptyWork << endl;

    // process cover
    if (!noverify) {
        vector<uintE> checkCover;
        checkCover.reserve(G.n);
        for (int i = 0; i < G.n; i++) {
            if (cover[i].test())
                checkCover.push_back(i);
        }
        if (!setcover::success<vertex>(G, checkCover)) std::cout << "Error occurred, but not aborting\n"; //abort();
    }
}

template <class vertex, typename MQ_Type>
void spawnTasksSMQ(graph<vertex>& G, MQ_Type &wl, int threadNum,
                atomic_flag* isElemCovered, atomic<uint32_t>* cardinality,
                atomic_flag* cover, termination_detector_2& detector,
                bool strict, bool batch, bool noverify=false)
{
    wl.init_thread(0);
    int cnt1 = 0, cnt2 = 0;
    // Queue each Set, prioritized by its current (initial) degree/cardinality
    for (size_t s = 0; s < G.n; s++) {
        uintE outCardinality = G.V[s].getOutDegree();
        uintE inCardinality = G.V[s].getInDegree();

        if (outCardinality > 0) {
            wl.insert(outCardinality, s);
            cardinality[s] = outCardinality;
        }

        if (outCardinality == 0) cnt1++;
        if (inCardinality == 0) cnt2++;
    }

    cout << cnt1 << " vertices with no outgoing edge\n";
    cout << cnt2 << " vertices with no incoming edge\n";

    stats threadStats[threadNum];
    for (int i = 0; i < threadNum;i++) {
        threadStats[i].emptyWork = 0;
    }

    auto begin = std::chrono::high_resolution_clock::now();

    vector<thread*> workers;
    cpu_set_t cpuset;
    for (int i = 1; i < threadNum; i++) {
        CPU_ZERO(&cpuset);
        uint64_t coreID = i;
        CPU_SET(coreID, &cpuset);

        std::thread *newThread = new std::thread(
            MQThreadTaskSMQ<vertex, MQ_Type>, ref(G),
            ref(wl), ref(isElemCovered),
            ref(cardinality), ref(cover),
            &threadStats[i], ref(detector), i
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
    MQThreadTaskSMQ<vertex, MQ_Type>(ref(G), ref(wl), ref(isElemCovered), ref(cardinality), ref(cover), &threadStats[0], ref(detector), 0);
    for (thread*& worker : workers) {
        worker->join();
        delete worker;
    }

    auto end = std::chrono::high_resolution_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(end-begin).count();
    //wl.stat();
    std::cout << "runtime_ms " << ms << "\n";

    uint64_t totalEmptyWork = 0;
    for (int i = 0; i < threadNum; i++) {
        totalEmptyWork += threadStats[i].emptyWork;
    }
    cout << "total empty work: " << totalEmptyWork << endl;

    // process cover
    if (!noverify) {
        vector<uintE> checkCover;
        checkCover.reserve(G.n);
        for (int i = 0; i < G.n; i++) {
            if (cover[i].test())
                checkCover.push_back(i);
        }
        if (!setcover::success<vertex>(G, checkCover)) std::cout << "Error occurred, but not aborting\n"; //abort();
    }
}

template <class vertex, typename MQ_Type>
void spawnTasksSpray(graph<vertex>& G, MQ_Type &wl, int threadNum,
                atomic_flag* isElemCovered, atomic<uint32_t>* cardinality,
                atomic_flag* cover, termination_detector_2& detector,
                thread_data_t *data, bool noverify=false)
{
    int cnt1 = 0, cnt2 = 0;
    // Queue each Set, prioritized by its current (initial) degree/cardinality
    for (size_t s = 0; s < G.n; s++) {
        uintE outCardinality = G.V[s].getOutDegree();
        uintE inCardinality = G.V[s].getInDegree();

        if (outCardinality > 0) {
            fraser_insert(wl, outCardinality, s);
            cardinality[s] = outCardinality;
        }

        if (outCardinality == 0) cnt1++;
        if (inCardinality == 0) cnt2++;
    }

    cout << cnt1 << " vertices with no outgoing edge\n";
    cout << cnt2 << " vertices with no incoming edge\n";

    stats threadStats[threadNum];
    for (int i = 0; i < threadNum;i++) {
        threadStats[i].emptyWork = 0;
    }

    auto begin = std::chrono::high_resolution_clock::now();

    vector<thread*> workers;
    cpu_set_t cpuset;
    for (int i = 1; i < threadNum; i++) {
        CPU_ZERO(&cpuset);
        uint64_t coreID = i;
        CPU_SET(coreID, &cpuset);

        std::thread *newThread = new std::thread(
            MQThreadTaskSpray<vertex, MQ_Type>, ref(G),
            ref(wl), ref(isElemCovered),
            ref(cardinality), ref(cover),
            &threadStats[i], ref(detector), i, data
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
    MQThreadTaskSpray<vertex, MQ_Type>(ref(G), ref(wl), ref(isElemCovered), ref(cardinality), ref(cover), &threadStats[0], ref(detector), 0, data);
    for (thread*& worker : workers) {
        worker->join();
        delete worker;
    }

    auto end = std::chrono::high_resolution_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(end-begin).count();
    //wl.stat();
    std::cout << "runtime_ms " << ms << "\n";

    uint64_t totalEmptyWork = 0;
    for (int i = 0; i < threadNum; i++) {
        totalEmptyWork += threadStats[i].emptyWork;
    }
    cout << "total empty work: " << totalEmptyWork << endl;

    // process cover
    if (!noverify) {
        vector<uintE> checkCover;
        checkCover.reserve(G.n);
        for (int i = 0; i < G.n; i++) {
            if (cover[i].test())
                checkCover.push_back(i);
        }
        if (!setcover::success<vertex>(G, checkCover)) std::cout << "Error occurred, but not aborting\n"; //abort();
    }
}

template <class vertex, typename MQ_Type>
void spawnTasksLinden(graph<vertex>& G, MQ_Type &wl, int threadNum,
                atomic_flag* isElemCovered, atomic<uint32_t>* cardinality,
                atomic_flag* cover, termination_detector_2& detector,
                bool noverify=false)
{
    int cnt1 = 0, cnt2 = 0;
    // Queue each Set, prioritized by its current (initial) degree/cardinality
    for (size_t s = 0; s < G.n; s++) {
        uintE outCardinality = G.V[s].getOutDegree();
        uintE inCardinality = G.V[s].getInDegree();

        if (outCardinality > 0) {
            insert(wl, outCardinality, s);
            cardinality[s] = outCardinality;
        }

        if (outCardinality == 0) cnt1++;
        if (inCardinality == 0) cnt2++;
    }

    cout << cnt1 << " vertices with no outgoing edge\n";
    cout << cnt2 << " vertices with no incoming edge\n";

    stats threadStats[threadNum];
    for (int i = 0; i < threadNum;i++) {
        threadStats[i].emptyWork = 0;
    }

    auto begin = std::chrono::high_resolution_clock::now();

    vector<thread*> workers;
    cpu_set_t cpuset;
    for (int i = 1; i < threadNum; i++) {
        CPU_ZERO(&cpuset);
        uint64_t coreID = i;
        CPU_SET(coreID, &cpuset);

        std::thread *newThread = new std::thread(
            MQThreadTaskLinden<vertex, MQ_Type>, ref(G),
            ref(wl), ref(isElemCovered),
            ref(cardinality), ref(cover),
            &threadStats[i], ref(detector), i
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
    MQThreadTaskLinden<vertex, MQ_Type>(ref(G), ref(wl), ref(isElemCovered), ref(cardinality), ref(cover), &threadStats[0], ref(detector), 0);
    for (thread*& worker : workers) {
        worker->join();
        delete worker;
    }

    auto end = std::chrono::high_resolution_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(end-begin).count();
    //wl.stat();
    std::cout << "runtime_ms " << ms << "\n";

    uint64_t totalEmptyWork = 0;
    for (int i = 0; i < threadNum; i++) {
        totalEmptyWork += threadStats[i].emptyWork;
    }
    cout << "total empty work: " << totalEmptyWork << endl;

    // process cover
    if (!noverify) {
        vector<uintE> checkCover;
        checkCover.reserve(G.n);
        for (int i = 0; i < G.n; i++) {
            if (cover[i].test())
                checkCover.push_back(i);
        }
        if (!setcover::success<vertex>(G, checkCover)) std::cout << "Error occurred, but not aborting\n"; //abort();
    }
}

template<class vertex, bool usePrefetch>
void initialize(graph<vertex>& GA, commandLine P) {
    string algoType = P.getOptionValue("-type", "MQBucket");
    int threadNum = P.getOptionIntValue("-threads", 1);
    int queueNum = P.getOptionIntValue("-queues", 2);
    int bucketNum = P.getOptionLongValue("-buckets", 64);
    int batchSizePop = P.getOptionIntValue("-batch1", 1);
    int batchSizePush = P.getOptionIntValue("-batch2", 1);
    int stickiness = P.getOptionIntValue("-stick", 1);
    bool noverify = P.getOptionValue("-noverify");
    int chunksize = P.getOptionIntValue("-chunksize", 128);
    int num_chunks = P.getOptionIntValue("-numchunks", 128);
    int max_batch_size = chunksize; //P.getOptionIntValue("-max_batch_size", 128);
    int delta = P.getOptionIntValue("-delta", 0);
    int strict = P.getOptionIntValue("-strict", 0);
    int batch = P.getOptionIntValue("-batch", 1);

    cout << "### Application: set-cover" << endl;
    cout << "### Graph: " << P.getArgument(0) << endl;
    cout << "### Workers: " << getWorkers() << endl;
    cout << "### n: " << GA.n << endl;
    cout << "### m: " << GA.m << endl;
    cout << "Type: " << algoType << endl;
    cout << "Thread num: " << threadNum << endl;

    if (algoType == "MQBucket" || algoType == "MQ") {
        cout << "MQ queue num: " << queueNum << endl;
        cout << "MQ batchsize pop: " << batchSizePop << endl;
        cout << "MQ batchsize push: " << batchSizePush << endl;
        cout << "MQ stickiness: " << stickiness << endl;
        cout << "MQ prefetch: " << usePrefetch << endl;
    }

    // initialize to 0
    atomic<uint32_t>* cardinality = new atomic<uint32_t>[GA.n]();
    atomic_flag* isElemCovered = new atomic_flag[GA.n]();
    atomic_flag* cover = new atomic_flag[GA.n]();

    std::function<void(uint32_t)> prefetcher = [&] (uint32_t v) -> void {
        __builtin_prefetch(&cardinality[v], 0, 3);
    };

    termination_detector_2 detector(threadNum);
    prio_tracker p_tracker(threadNum, delta);

    if (algoType == "MQBucket") {
        cout << "MQ bucket num: " << bucketNum << endl;
        uintE m = 0;
        for (size_t s = 0; s < GA.n; s++) {
            m = max(m, GA.V[s].getOutDegree());
        }
        cout << "max cardinality = " << m << "\n";

        std::function<mbq::BucketID(uint32_t)> getBucketID = [&] (uint32_t v) -> mbq::BucketID {
            uint32_t card = cardinality[v].load(std::memory_order_acquire);
            return card;
        };
        using MQ_Bucket = mbq::MultiBucketQueue<
            decltype(getBucketID), decltype(prefetcher),
            less<uintE>, uintE, uintE, usePrefetch>;
        MQ_Bucket wl(getBucketID, prefetcher, queueNum, threadNum, 0,
                 bucketNum, batchSizePop, batchSizePush, mbq::decreasing, stickiness, m);
        spawnTasks<vertex, MQ_Bucket>(GA, wl, threadNum, isElemCovered, cardinality, cover, detector, noverify);

    } else if (algoType == "MQ") {
        using MQ = mbq::MultiQueue<decltype(prefetcher), less<PQElement>, uintE, uintE, usePrefetch>;
        MQ wl(prefetcher, queueNum, threadNum, batchSizePop, batchSizePush, stickiness);
        spawnTasks<vertex, MQ>(GA, wl, threadNum, isElemCovered, cardinality, cover, detector, noverify);
    } else if (algoType == "SkiphashPQ") {
        #define _ALG eager_noext_c1_t
        #define _OREC orec_po_t
        #define _CLOCK rdtscp_clock_t

        using descriptor = _ALG<_OREC, clock_policy::_CLOCK>;
        // using OPTSTM = _ALG<_OREC, clock_policy::_CLOCK>;
        using map = skiphash_pq_relaxed<uintE, uintE, descriptor>; // todo: should order be decreasing ??
        auto *me = new descriptor();

        config_t cfg;
        cfg.chunksize = chunksize;
        cfg.num_queues = num_chunks;
        cfg.max_batch_size = max_batch_size;
        cfg.delta = delta;
        cfg.order = 1; // order = DECREASING

        std::cout << "cfg.max_levels: " << static_cast<int>(cfg.max_levels) << "\n";
        std::cout << "cfg.buckets: " << cfg.buckets << "\n";
        std::cout << "cfg.chunksize: " << cfg.chunksize << "\n";
        std::cout << "cfg.num_queues: " << cfg.num_queues << "\n";
        std::cout << "cfg.max_batch_size: " << cfg.max_batch_size << "\n";
        std::cout << "cfg.delta: " << cfg.delta << "\n";
        
        map pq(me, &cfg);
        spawnTasksSTM<vertex, map, descriptor>(GA, pq, threadNum, isElemCovered, cardinality, cover, detector, strict, batch, p_tracker, noverify);
    } else if (algoType == "SMQ") {
        const size_t steal_probability = 8; // 1/8 probability of stealing
        //const size_t steal_batch_size = 128; // size of batch to steal
        const size_t steal_batch_size = 32; // size of batch to steal

        using smq_t = smq_ns::StealingMultiQueue<std::pair<uint32_t,uint32_t>,uint32_t,steal_probability,steal_batch_size,true>;
        auto smq_ds = smq_t(threadNum);
        spawnTasksSMQ<vertex, smq_t>(GA, smq_ds, threadNum, isElemCovered, cardinality, cover, detector, strict, batch, noverify);
    } else if (algoType == "Spray") {
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
        spawnTasksSpray<vertex, sl_intset_t*>(GA, sl_spray, threadNum, isElemCovered, cardinality, cover, detector, data, noverify);
    } else if (algoType == "Linden") {
        int max_offset = 32;
        _init_gc_subsystem();
        pq_t * linden_pq = pq_init(max_offset, 1);
        spawnTasksLinden<vertex, pq_t *>(GA, linden_pq, threadNum, isElemCovered, cardinality, cover, detector, noverify);
    } else if (algoType == "PIPQ") {
        int heap_list_size = 10000000;
        int cntr_tsh = 10;
        int cntr_max = 100;

        using pipq_t = pq_ns::pipq<uint32_t>;

        //std::cout << "counter max: " << counter_max << ", counter tsh: " << counter_tsh << "\n";
        auto numa_pq_ds = pipq_t(heap_list_size, threadNum, cntr_tsh, cntr_max, pinning);
        numa_pq_ds.PQInit();
    
    } else {
        cout << "Invalid type!\n";
    }
    delete [] cover;
    delete [] cardinality;
    delete [] isElemCovered;
}

template <class vertex>
void Compute(graph<vertex>& GA, commandLine P) {
    bool usePrefetch = P.getOptionValue("-prefetch");
    if (usePrefetch) initialize<vertex, true>(GA, P);
    else initialize<vertex, false>(GA, P);
}
