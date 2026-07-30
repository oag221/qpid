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

#include "message.h"
#include "mrf.h"

#include <smq_impl.h>
#include "../galois/lonestar/analytics/cpu/termination_helper.h"

namespace smq_rbp {

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

template<typename MQ_Bucket>
static void thread_task(int tid, MRF* mrf, double sensitivity, stat *stats, MQ_Bucket& pq, termination_detector_2& detector) {
    uint64_t updates = 0;
    uint64_t skips = 0;
    uint64_t it = 0;
    using extract_ret_t = std::optional<std::pair<uint32_t,Message*>>;
    auto call_extract = [&]() {
        return pq.extract_min();
    };
    if (tid) pq.init_thread(tid);

    while (true) {
        Message *m;
        uint32_t b;

        auto ret_term = try_extract<extract_ret_t>(detector, call_extract); // handles termination detection
        if (!ret_term) break; // TERMINATE
        std::tie(b, m) = ret_term.value();
        //std::cout << "Removed " << b << "\n";
        it++;

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
                pq.insert(prio, m);
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
                            pq.insert(prio, affected);
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

template<bool usePrefetch>
void spawnTasks(double sensitivity, int threadNum) {
    const size_t steal_probability = 8; // 1/8 probability of stealing
    const size_t steal_batch_size = 64;
    using smq_t = smq_ns::StealingMultiQueue<std::pair<uint32_t,Message*>,Message*,steal_probability,steal_batch_size,true>;
    int order = 1;
    auto smq_ds = smq_t(threadNum, order);

    termination_detector_2 detector(threadNum);

    smq_ds.init_thread(0);

    for (Message& message: *messages) {
        double p = priority(&message);
        if (p > sensitivity) {
            priorities[(id(&message))] = p;
            uint32_t prio = p * MULT_VAL;
            smq_ds.insert(prio, &message);
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
            thread_task<smq_t>, i, std::ref(mrf), 
            std::ref(sensitivity), &stats[i], std::ref(smq_ds), std::ref(detector)
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
    thread_task<smq_t>(0, mrf, sensitivity, &stats[0], smq_ds, std::ref(detector));
    for (std::thread*& worker : workers) {
        worker->join();
        delete worker;
    }

    auto endTime = std::chrono::high_resolution_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(endTime-startTime);

    //pq.stat();
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

void solve(MRF* mrf, double sensitivity,
           std::vector<std::array<double,2> >* answer,
           int threadNum) {
    std::cout << "Running a relaxed version of residual belief "
              << "propagation with SMQ"
              << std::endl;
    std::cout << "threads = " << threadNum << "\n";

    smq_rbp::messages = &mrf->getMessages();
    smq_rbp::mrf = mrf;
    smq_rbp::baseMessage = messages->data();
    smq_rbp::locks = new std::vector<std::mutex>(mrf->getNodes());
    smq_rbp::priorities = new std::atomic<double>[messages->size()]();

    spawnTasks<false>(sensitivity, threadNum);

    mrf->getNodeProbabilities(answer);

    delete locks;
}



}