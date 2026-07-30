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

#include "message.h"
#include "mrf.h"


#include "skiphash_pq_relaxed.h"
#include "optstm2/eager_noext_c1.h"
#include "../galois/lonestar/analytics/cpu/termination_helper.h"
#include "../galois/lonestar/analytics/cpu/config.h"
#include "../priority_tracker.h"

OPTSTM2_GLOBALS_INITIALIZER;

namespace skiphash_rbp {

// This algorithm is based on relaxed-rbp.cpp with filtering to reduce
// queue pressure.
// This version is implemented with the heap-based MultiQueue as the scheduler.

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
constexpr static const unsigned MAX_PREFETCH = 64;

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

template<typename MQ_I, typename descriptor>
static void thread_task(MRF* mrf,
                        double sensitivity,
                        stat *stats,
                        MQ_I &pq,
                        termination_detector_2& detector,
                        prio_tracker& p_tracker,
                        int tid){
    uint64_t updates = 0;
    uint64_t skips = 0;
    uint64_t it = 0;
    auto* me = new descriptor();
    if (tid) pq.init_thread(me, tid);
    else pq.re_init_thread(me);
    
    using extract_ret_t = std::optional<std::pair<uint32_t,Message*>>;
    auto call_extract = [&]() {
        me->op_begin();
        auto ret = pq.extract_min(me); 
        me->op_end();
        return ret;
    };

    #ifdef PROFILING_SERIAL
    int cnt = 0;
    int print_rate = 2000;
    if (tid > 0) std::cout << "WARNING: PROFILING is defined, but using >1 thread - subsequent call to dump_ht() is serial\n";
    #endif

    while (true) {
        Message *m;
        uint32_t b;

        auto ret_term = try_extract<extract_ret_t>(detector, call_extract); // handles termination detection
        if (!ret_term) break; // TERMINATE
        std::tie(b, m) = ret_term.value();
        it++;
        //std::cout << "Removed " << b << "\n";

        #ifdef PROFILING_SERIAL
        if (++cnt % print_rate == 0) {
            std::cout << "(cnt=" << cnt << ")\n";
            pq.dump_ht();
            std::cout << "\n\n";
        }
        #endif

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
                me->op_begin();
                pq.insert(me, prio, m);
                me->op_end();
                #ifdef PROFILING
                p_tracker.append_prio(tid, prio);
                #endif
                //std::cout << "Inserted " << prio << " -> " << ((prio <= 0) ? 0 : 31 - __builtin_clz(prio)) << "\n";
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
                            me->op_begin();
                            pq.insert(me, prio, affected);
                            me->op_end();
                            #ifdef PROFILING
                            p_tracker.append_prio(tid, prio);
                            #endif
                            //std::cout << "Inserted " << prio << " -> " << ((prio <= 0) ? 0 : 31 - __builtin_clz(prio)) << "\n";
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
    pq.thread_terminate();
}

template<typename MQ_I, typename descriptor>
static void thread_task_strict(MRF* mrf,
                        double sensitivity,
                        stat *stats,
                        MQ_I &pq,
                        termination_detector_2& detector,
                        prio_tracker& p_tracker,
                        int tid){
    uint64_t updates = 0;
    uint64_t skips = 0;
    uint64_t it = 0;
    auto* me = new descriptor();
    if (tid) pq.init_thread(me, tid);
    else pq.re_init_thread(me);

    #ifdef PROFILING_SERIAL
    int cnt = 0;
    int print_rate = 2000;
    if (tid > 0) std::cout << "WARNING: PROFILING is defined, but using >1 thread - subsequent call to dump_ht() is serial\n";
    #endif

    using extract_ret_t = std::optional<std::pair<uint32_t,Message*>>;
    auto call_extract = [&]() {
        me->op_begin();
        auto ret = pq.extract_min_strict(me); 
        me->op_end();
        return ret;
    };

    while (true) {
        Message *m;
        uint32_t b;

        auto ret_term = try_extract<extract_ret_t>(detector, call_extract); // handles termination detection
        if (!ret_term) break; // TERMINATE
        std::tie(b, m) = ret_term.value();
        it++;
        //std::cout << "Removed " << b << "\n";

        #ifdef PROFILING_SERIAL
        if (++cnt % print_rate == 0) {
            std::cout << "(cnt=" << cnt << ")\n";
            pq.dump_ht();
            std::cout << "\n\n";
        }
        #endif

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
                me->op_begin();
                pq.insert(me, prio, m);
                me->op_end();
                #ifdef PROFILING
                p_tracker.append_prio(tid, prio);
                #endif
                //std::cout << "Inserted " << prio << " -> " << ((prio <= 0) ? 0 : 31 - __builtin_clz(prio)) << "\n";
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
                            me->op_begin();
                            pq.insert(me, prio, affected);
                            me->op_end();
                            #ifdef PROFILING
                            p_tracker.append_prio(tid, prio);
                            #endif
                            //std::cout << "Inserted " << prio << " -> " << ((prio <= 0) ? 0 : 31 - __builtin_clz(prio)) << "\n";
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

template<typename MQ_I, typename descriptor>
static void thread_task_batch_ins(MRF* mrf,
                        double sensitivity,
                        stat *stats,
                        MQ_I &pq,
                        termination_detector_2& detector,
                        prio_tracker& p_tracker,
                        int tid){
    uint64_t updates = 0;
    uint64_t skips = 0;
    uint64_t it = 0;

    auto* me = new descriptor();
    if (tid) pq.init_thread(me, tid);
    else pq.re_init_thread(me);
    
    using extract_ret_t = std::optional<std::pair<uint32_t,Message*>>;
    auto call_extract = [&]() {
        me->op_begin();
        auto ret = pq.extract_min(me); 
        me->op_end();
        return ret;
    };

    while (true) {
        Message *m;
        uint32_t b;

        auto ret_term = try_extract<extract_ret_t>(detector, call_extract); // handles termination detection
        if (!ret_term) break; // TERMINATE
        std::tie(b, m) = ret_term.value();
        it++;
        //std::cout << "Removed " << b << " -> " << ((b <= 0) ? 0 : 31 - __builtin_clz(b)) << "\n";

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
                me->op_begin();
                pq.insert(me, prio, m);
                me->op_end();
                #ifdef PROFILING
                p_tracker.append_prio(tid, prio);
                #endif
                //std::cout << "Inserted " << prio << " -> " << ((prio <= 0) ? 0 : 31 - __builtin_clz(prio)) << "\n";
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
                            me->op_begin();
                            pq.insert_batch(me, prio, affected);
                            me->op_end();
                            #ifdef PROFILING
                            p_tracker.append_prio(tid, prio);
                            #endif
                            //std::cout << "Inserted " << prio << " -> " << ((prio <= 0) ? 0 : 31 - __builtin_clz(prio)) << "\n";
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
            me->op_begin();
            pq.flush_batch(me, true);
            me->op_end();

            updates++;
        }

        locks->at(mi).unlock();
        locks->at(mj).unlock();
    }

    stats->iters=it;
    stats->updates=updates;
    stats->skips=skips;
}

void spawnTasks(double sensitivity, int threadNum, int queueNum, int chunkSize, int delta, bool batch_ins, bool strict) {
    using PQElement = std::tuple<double, Message*>;
    std::function<void(Message *)> prefetcher = [&] (Message *m) -> void {
        __builtin_prefetch(&priorities[id(m)], 1, 3);
    };

    termination_detector_2 detector(threadNum);
    prio_tracker p_tracker(threadNum, delta);

    #define _ALG eager_noext_c1_t
    #define _OREC orec_po_t
    #define _CLOCK rdtscp_clock_t

    using descriptor = _ALG<_OREC, clock_policy::_CLOCK>;
    using map = skiphash_pq_relaxed<uint32_t, Message*, descriptor>;
    auto *me = new descriptor();

    config_t cfg;
    cfg.threads = threadNum;
    cfg.order = 1; // Decreasing order!

    cfg.chunksize = chunkSize;
    cfg.num_queues = queueNum;
    cfg.max_batch_size = chunkSize;
    cfg.delta = delta;

    std::cout << "cfg.max_levels: " << static_cast<int>(cfg.max_levels) << "\n";
    std::cout << "cfg.buckets: " << cfg.buckets << "\n";
    std::cout << "cfg.chunksize: " << cfg.chunksize << "\n";
    std::cout << "cfg.num_queues: " << cfg.num_queues << "\n";
    std::cout << "cfg.max_batch_size: " << cfg.max_batch_size << "\n";
    std::cout << "cfg.delta: " << cfg.delta << "\n";
    std::cout << "batch_ins: " << batch_ins << "\n";
    
    std::cout << "Initializing pq...\n";
    map pq(me, &cfg);
    pq.init_thread(me, 0);
    std::cout << "Done initializing pq.\n";

    for (Message& message: *messages) {
        double prio = priority(&message);
        if (prio > sensitivity) {
            priorities[(id(&message))] = prio;
            uint32_t p_ = prio * MULT_VAL;
            me->op_begin();
            pq.insert(me, p_, &message);
            me->op_end();
            #ifdef PROFILING
            p_tracker.append_prio(0, p_);
            #endif
        }
    }

    #ifdef PROFILING
    std::cout << "AFTER initial inserts:\n";
    p_tracker.print_sum_prios();
    #endif

    std::vector<std::thread*> workers;
    stat stats[threadNum];

    auto startTime = std::chrono::high_resolution_clock::now();

    cpu_set_t cpuset;
    for (uint64_t i = 1; i < threadNum; i++) {
        CPU_ZERO(&cpuset);
        uint64_t coreID = i;
        CPU_SET(coreID, &cpuset);
        std::thread *newThread;
        if (batch_ins) {
            newThread = new std::thread(
                thread_task_batch_ins<map, descriptor>, std::ref(mrf), 
                std::ref(sensitivity), &stats[i], std::ref(pq), std::ref(detector), std::ref(p_tracker), i
            );
        } else if (strict) {
            newThread = new std::thread(
                thread_task_strict<map, descriptor>, std::ref(mrf), 
                std::ref(sensitivity), &stats[i], std::ref(pq), std::ref(detector), std::ref(p_tracker), i
            );
        } else {
            newThread = new std::thread(
                thread_task<map, descriptor>, std::ref(mrf), 
                std::ref(sensitivity), &stats[i], std::ref(pq), std::ref(detector), std::ref(p_tracker), i
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
    if (batch_ins) thread_task_batch_ins<map, descriptor>(mrf, sensitivity, &stats[0], pq, std::ref(detector), p_tracker, 0);
    else if (strict) thread_task_strict<map, descriptor>(mrf, sensitivity, &stats[0], pq, std::ref(detector), p_tracker, 0);
    else thread_task<map, descriptor>(mrf, sensitivity, &stats[0], pq, std::ref(detector), p_tracker, 0);
    for (std::thread*& worker : workers) {
        worker->join();
        delete worker;
    }

    auto endTime = std::chrono::high_resolution_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(endTime-startTime);

    std::cout << "AFTER experiment:\n";
    pq.dump(me);
    
    std::cout << "runtime_ms " << ms.count() << std::endl;

    #ifdef PROFILING
    p_tracker.print_sum_prios();
    #endif

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
           int threadNum, int queueNum, int chunkSize, int delta,
           bool batch_ins, bool strict) {
    std::cout << "Running a relaxed version of residual belief "
              << "propagation with heap-based multiqueue"
              << std::endl;
    std::cout << "threads = " << threadNum << "\n";
    std::cout << "queues = " << queueNum << "\n";
    std::cout << "chunkSize = " << chunkSize << "\n";
    std::cout << "delta = " << delta << "\n";
    std::cout << "strict = " << strict << "\n";
    std::cout << "batch_ins = " << batch_ins << "\n";

    skiphash_rbp::messages = &mrf->getMessages();
    skiphash_rbp::mrf = mrf;
    skiphash_rbp::baseMessage = messages->data();
    skiphash_rbp::locks = new std::vector<std::mutex>(mrf->getNodes());
    skiphash_rbp::priorities = new std::atomic<double>[messages->size()]();

    spawnTasks(sensitivity, threadNum, queueNum, chunkSize, delta, batch_ins, strict);

    mrf->getNodeProbabilities(answer);

    delete locks;
}

}