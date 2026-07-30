#include <array>
#include <cassert>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <queue>
#include <tuple>
#include <vector>
#include <chrono>
#include <atomic>
#include <thread>
#include <mutex>
#include <algorithm>
#include <bits/stdc++.h>

#include "message.h"
#include "mrf.h"

#include <include-pipq/pipq_impl.h> // pipq

#include <include-linden/linden.h> // linden
#include "gc/ptst.h"

#include "intset.h" // spray
#include <common/include/random.h>

#include "../galois/lonestar/analytics/cpu/termination_helper.h"

const int pinning[96] = {0,2,4,6,8,10,12,14,16,18,20,22,24,26,28,30,32,34,36,38,40,42,44,46,1,3,5,7,9,11,13,15,17,19,21,23,25,27,29,31,33,35,37,39,41,43,45,47,48,50,52,54,56,58,60,62,64,66,68,70,72,74,76,78,80,82,84,86,88,90,92,94,49,51,53,55,57,59,61,63,65,67,69,71,73,75,77,79,81,83,85,87,89,91,93,95};

alignas(64) extern uint8_t levelmax[64];
__thread unsigned long *seeds;

namespace competitors_rbp {

// This algorithm is based on relaxed-rbp.cpp with filtering to reduce
// queue pressure.
// This version is implemented with Multi Bucket Queue as the scheduler.
// The priorities are converted to integer values and shifted by a delta value
// to map to a priority level.

static std::vector<Message>* messages;

struct stat {
    uint64_t iters = 0;
    uint64_t updates = 0;
    uint64_t skips = 0;
};

static MRF* mrf;
static const Message* baseMessage;
static std::vector<std::mutex>* locks;
static std::atomic<double>* priorities;

static constexpr uint64_t MULT_VAL = 1e6;
static constexpr double INV_MULT_VAL = 1.0 / MULT_VAL;

static inline uint64_t id(const Message* m) {
    return std::distance(baseMessage, m);
}

template <class T>
static inline double priority(const Message* m, T futureMessage) {
    return utils::distance(m->logMu, futureMessage);
}

static inline double priority(const Message* m) {
    return priority(m, mrf->getFutureMessage(*m));
}

template<typename MQ_Bucket, typename InitFunc, typename InsFunc, typename ExtractFunc>
static void thread_task(int tid, MRF* mrf, double sensitivity, stat *stats, MQ_Bucket& pq, termination_detector_2& detector, InitFunc initFunc, InsFunc insFunc, ExtractFunc extractFunc) {
    uint64_t updates = 0;
    uint64_t skips = 0;
    uint64_t it = 0;
    using extract_ret_t = std::optional<std::pair<uint32_t,Message*>>;
    if (tid) initFunc(tid);

    while (true) {
        Message *m;
        uint32_t b;

        auto ret_term = try_extract<extract_ret_t>(detector, extractFunc); // handles termination detection
        if (!ret_term) break; // TERMINATE
        std::tie(b, m) = ret_term.value();
        it++;
        //std::cout << "Removed " << b << "\n";

        // compute the pushedPrio based on poppedBkt
        double pushedPrio = (double)b * INV_MULT_VAL;
        

        uint64_t mi = std::min(m->i, m->j);
        uint64_t mj = std::max(m->i, m->j);
        locks->at(mi).lock();
        locks->at(mj).lock();

        uint64_t mID = id(m);
        double curPrio = priorities[mID].load(std::memory_order_relaxed);
        if (curPrio < pushedPrio) {
            // Outdated, re-insert with the current priority
            if (curPrio > sensitivity) {
                uint32_t prio = curPrio * MULT_VAL;
                // pq.ins_wrapper(prio, m);
                insFunc(prio, m);
                //std::cout << "Inserted " << prio << "\n";
            }
            skips++;

        } else {
            // Up to date, perform updates and enqueue messages to neighbors
            auto futureMessage = mrf->getFutureMessage(*m);
            mrf->updateMessage(*m, futureMessage);
            priorities[mID] = 0.0;

            auto fromJ = mrf->getMessagesFrom(m->j);
            for (Message* affected : fromJ) {
                if (affected->j == m->i) {
                    continue;
                }

                uint64_t affID = id(affected);
                double affNewPrio = priority(affected);
                double affCurPrio = priorities[affID].load(std::memory_order_relaxed);
                
                // only update the affected's priority if affNewPrio is more prioritized
                if (affCurPrio < affNewPrio) {
                    if (affNewPrio > sensitivity) {
                        while (affCurPrio < affNewPrio &&
                            !priorities[affID].compare_exchange_weak(
                                affCurPrio, affNewPrio, 
                                std::memory_order_acq_rel, std::memory_order_relaxed));
                        // if swapped, push the msg into the queue again
                        if (affCurPrio < affNewPrio) {
                            uint32_t prio = affNewPrio * MULT_VAL;
                            //pq.ins_wrapper(prio, affected);
                            insFunc(prio, affected);
                            //std::cout << "Inserted " << prio << "\n";
                        }
                    }
                } else {
                    if (affCurPrio > sensitivity) {
                        while (affCurPrio < affNewPrio &&
                            !priorities[affID].compare_exchange_weak(
                                affCurPrio, affNewPrio,
                                std::memory_order_acq_rel, std::memory_order_relaxed));
                    }
                }
            }
            updates++;
        }

        locks->at(mi).unlock();
        locks->at(mj).unlock();
    }

    stats->iters=it;
    stats->updates=updates;
    stats->skips=skips;
}

template<typename MQ_Bucket, typename InitFunc, typename InsFunc>
static void thread_task_spray(int tid, MRF* mrf, double sensitivity, stat *stats, MQ_Bucket& pq, termination_detector_2& detector, InitFunc initFunc, InsFunc insFunc, thread_data_t* data) {
    uint64_t updates = 0;
    uint64_t skips = 0;
    uint64_t it = 0;
    using extract_ret_t = std::optional<std::pair<uint32_t,Message*>>;
    thread_data_t* data_item = &(data[tid]);
    auto call_extract = [&]() {
        return spray_delete_min_key(pq, data_item); //! 1 is ordering, right?
    };
    initFunc(tid);

    while (true) {
        Message *m;
        uint32_t b;

        auto ret_term = try_extract<extract_ret_t>(detector, call_extract); // handles termination detection
        if (!ret_term) break; // TERMINATE
        std::tie(b, m) = ret_term.value();
        it++;
        std::cout << "removed " << b << "\n";

        // compute the pushedPrio based on poppedBkt
        double pushedPrio = (double)(INT_MAX - b) * INV_MULT_VAL;

        uint64_t mi = std::min(m->i, m->j);
        uint64_t mj = std::max(m->i, m->j);
        locks->at(mi).lock();
        locks->at(mj).lock();

        uint64_t mID = id(m);
        double curPrio = priorities[mID].load(std::memory_order_relaxed);
        if (curPrio < pushedPrio) {
            // Outdated, re-insert with the current priority
            if (curPrio > sensitivity) {
                std::cout << "INT_MAX=" << INT_MAX << "\n";
                std::cout << "(curPrio * MULT_VAL)=" << (curPrio * MULT_VAL) << "\n";
                uint32_t prio = INT_MAX - static_cast<uint32_t>(std::round(curPrio * MULT_VAL));
                //pq->insert(prio, m);
                insFunc(prio, m);
                std::cout << "~~inserted " << prio << " (cur_prio=" << curPrio << ", pushedPrio=" << pushedPrio << ")\n\n";
                //std::cout << "Inserted " << prio << "\n";
            }
            skips++;

        } else {
            // Up to date, perform updates and enqueue messages to neighbors
            auto futureMessage = mrf->getFutureMessage(*m);
            mrf->updateMessage(*m, futureMessage);
            priorities[mID] = 0.0;

            auto fromJ = mrf->getMessagesFrom(m->j);
            for (Message* affected : fromJ) {
                if (affected->j == m->i) {
                    continue;
                }

                uint64_t affID = id(affected);
                double affNewPrio = priority(affected);
                double affCurPrio = priorities[affID].load(std::memory_order_relaxed);
                
                // only update the affected's priority if affNewPrio is more prioritized
                if (affCurPrio < affNewPrio) {
                    if (affNewPrio > sensitivity) {
                        while (affCurPrio < affNewPrio &&
                            !priorities[affID].compare_exchange_weak(
                                affCurPrio, affNewPrio, 
                                std::memory_order_acq_rel, std::memory_order_relaxed));
                        // if swapped, push the msg into the queue again
                        if (affCurPrio < affNewPrio) {
                            uint32_t prio = INT_MAX - (affNewPrio * MULT_VAL);
                            insFunc(prio, affected);
                            std::cout << "inserted " << prio << "\n";
                        }
                    }
                } else {
                    if (affCurPrio > sensitivity) {
                        while (affCurPrio < affNewPrio &&
                            !priorities[affID].compare_exchange_weak(
                                affCurPrio, affNewPrio,
                                std::memory_order_acq_rel, std::memory_order_relaxed));
                    }
                }
            }
            updates++;
        }

        locks->at(mi).unlock();
        locks->at(mj).unlock();
    }

    stats->iters=it;
    stats->updates=updates;
    stats->skips=skips;
}

template<bool usePrefetch, typename PQ_Type, typename InitFunc, typename InsFunc, typename ExtractFunc>
void spawnTasks(PQ_Type& pq, double sensitivity, int threadNum, InitFunc initFunc, InsFunc insFunc, ExtractFunc extractFunc) {
    termination_detector_2 detector(threadNum);
    initFunc(0);

    for (Message& message: *messages) {
        double p = priority(&message);
        if (p > sensitivity) {
            priorities[(id(&message))] = p;
            uint32_t prio = p * MULT_VAL;
            insFunc(prio, &message);
        }
    }

    std::vector<std::thread*> workers;
    stat stats[threadNum];

    auto startTime = std::chrono::high_resolution_clock::now();

    cpu_set_t cpuset;
    for (uint64_t i = 1; i < threadNum; i++) {
        CPU_ZERO(&cpuset);
        uint64_t coreID = i;
        CPU_SET(coreID, &cpuset);
        std::thread *newThread = new std::thread(
            thread_task<PQ_Type, InitFunc, InsFunc, ExtractFunc>, i, std::ref(mrf), 
            std::ref(sensitivity), &stats[i], std::ref(pq), std::ref(detector), initFunc, insFunc, extractFunc
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
    thread_task<PQ_Type, InitFunc, InsFunc, ExtractFunc>(0, mrf, sensitivity, &stats[0], pq, std::ref(detector), initFunc, insFunc, extractFunc);
    for (std::thread*& worker : workers) {
        worker->join();
        delete worker;
    }

    auto endTime = std::chrono::high_resolution_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(endTime-startTime);

    std::cout << "runtime_ms " << ms.count() << std::endl;

    uint64_t totalIters = 0;
    uint64_t totalUpdates = 0;
    uint64_t totalSkips = 0;
    for (int i = 0; i < threadNum; i++) {
        totalIters += stats[i].iters;
        totalUpdates += stats[i].updates;
        totalSkips += stats[i].skips;
    }

    std::cout << "totalIters = " << totalIters << std::endl;
    std::cout << "totalUpdates = " << totalUpdates << std::endl;
    std::cout << "totalSkips = " << totalSkips << std::endl;
}

template<bool usePrefetch>
void spawnTasksSpray(double sensitivity, int threadNum) {
    std::cout << "Running Spraylist\n";
    *levelmax = 32;
    thread_data_t *data = (thread_data_t *)malloc(threadNum * sizeof(thread_data_t));
    for (int i = 0; i < threadNum; i++) {
      data[i].seed = rand();
      data[i].seed2 = rand();
      data[i].nb_threads = threadNum;
    }
    seeds = seed_rand();
    sl_intset_t * sl_spray = sl_set_new(); // todo: what is the 1 ??? order?

    // Lambda's
    auto call_init_spray = [&](int tid) {
      if (tid) seeds = seed_rand();
    };
    auto call_insert_spray = [&](uint32_t p, Message* v) {
      fraser_insert(sl_spray, p, v);
    };

    termination_detector_2 detector(threadNum);

    uint32_t largest = 0;
    uint32_t smallest = INT_MAX;
    for (Message& message: *messages) {
        double p = priority(&message);
        if (p > sensitivity) {
            priorities[(id(&message))] = p;
            uint32_t prio = INT_MAX - (p * MULT_VAL);
            fraser_insert(sl_spray, prio, &message);
            
            //sl_spray.insert(prio, &message);

            largest = std::max(largest, prio);
            smallest = std::min(smallest, prio);
        }
    }

    std::cout << "[prefill] largest " << largest << "\n";
    std::cout << "[prefill] smallest " << smallest << "\n";


    std::vector<std::thread*> workers;
    stat stats[threadNum];

    auto startTime = std::chrono::high_resolution_clock::now();

    cpu_set_t cpuset;
    for (uint64_t i = 1; i < threadNum; i++) {
        CPU_ZERO(&cpuset);
        uint64_t coreID = i;
        CPU_SET(coreID, &cpuset);
        std::thread *newThread = new std::thread(
            thread_task_spray<sl_intset_t *, decltype(call_init_spray), decltype(call_insert_spray)>, i, std::ref(mrf), 
            std::ref(sensitivity), &stats[i], std::ref(sl_spray), std::ref(detector), call_init_spray, call_insert_spray, data
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
    thread_task_spray<sl_intset_t *>(0, mrf, sensitivity, &stats[0], sl_spray, std::ref(detector), call_init_spray, call_insert_spray, data);
    for (std::thread*& worker : workers) {
        worker->join();
        delete worker;
    }

    auto endTime = std::chrono::high_resolution_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(endTime-startTime);

    std::cout << "runtime_ms " << ms.count() << std::endl;

    uint64_t totalIters = 0;
    uint64_t totalUpdates = 0;
    uint64_t totalSkips = 0;
    for (int i = 0; i < threadNum; i++) {
        totalIters += stats[i].iters;
        totalUpdates += stats[i].updates;
        totalSkips += stats[i].skips;
    }

    std::cout << "totalIters = " << totalIters << std::endl;
    std::cout << "totalUpdates = " << totalUpdates << std::endl;
    std::cout << "totalSkips = " << totalSkips << std::endl;
}

void solve(std::string qType, MRF* mrf, double sensitivity,
           std::vector<std::array<double,2> >* answer,
           int threadNum) {
    std::cout << "Running a relaxed version of residual belief "
              << "propagation with "
              << qType
              << std::endl;
    std::cout << "threads = " << threadNum << "\n";

    competitors_rbp::messages = &mrf->getMessages();
    competitors_rbp::mrf = mrf;
    competitors_rbp::baseMessage = messages->data();
    competitors_rbp::locks = new std::vector<std::mutex>(mrf->getNodes());
    competitors_rbp::priorities = new std::atomic<double>[messages->size()]();

    if (qType == "spray") {
        spawnTasksSpray<false>(sensitivity, threadNum);
    } else if (qType == "linden") {
        std::cout << "running linden\n";
        int max_offset = 32;
        _init_gc_subsystem();
        pq_t * linden_pq = pq_init(max_offset, 1); // todo: make sure linden stores floats

        // Lambda's
        auto call_init_lind = [](int tid){};
        auto call_insert_lind = [&](uint32_t p, Message* v) {
            insert(linden_pq, p, v);
        };
        auto call_extract_lind = [&]() {
            return extract_min(linden_pq);
        };

        spawnTasks<false, pq_t *>(linden_pq, sensitivity, threadNum, call_init_lind, call_insert_lind, call_extract_lind);
    } else if (qType == "pipq") {
        std::cout << "running pipq\n";
        int heap_list_size = 1000000;
        int cntr_tsh = 10;
        int cntr_max = 100;

        using pipq_t = pq_ns::pipq<Message*>;

        auto numa_pq_ds = pipq_t(heap_list_size, threadNum, cntr_tsh, cntr_max, pinning);
        numa_pq_ds.PQInit();

        // Lambda's
        auto call_init_pipq = [&](int tid) {
            numa_pq_ds.init_thread(tid);
        };
        auto call_insert_pipq = [&](uint32_t p, Message* v) {
            numa_pq_ds.insert(p, v);
        };
        auto call_extract_pipq = [&]() {
            return numa_pq_ds.extract_min();
        };
        spawnTasks<false, pipq_t>(numa_pq_ds, sensitivity, threadNum, call_init_pipq, call_insert_pipq, call_extract_pipq);
    }

    mrf->getNodeProbabilities(answer);

    delete locks;
}

}