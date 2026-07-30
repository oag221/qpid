#include <iostream>
#include <vector>
#include <cmath>
#include <map>
#include <iomanip>
#include <thread>
#include <random>

#include "data_structures.h"

#ifdef QPID
OPTSTM2_GLOBALS_INITIALIZER;
#elif defined(SPRAYLIST)
alignas(64) extern uint8_t levelmax[64];
__thread unsigned long *seeds;
#elif defined(LINDEN)
__thread unsigned long *seeds;
#endif

const int pinning[96] = {0,2,4,6,8,10,12,14,16,18,20,22,24,26,28,30,32,34,36,38,40,42,44,46,1,3,5,7,9,11,13,15,17,19,21,23,25,27,29,31,33,35,37,39,41,43,45,47,48,50,52,54,56,58,60,62,64,66,68,70,72,74,76,78,80,82,84,86,88,90,92,94,49,51,53,55,57,59,61,63,65,67,69,71,73,75,77,79,81,83,85,87,89,91,93,95};

// Structure to hold results per thread without sharing cache lines
struct alignas(64) ThreadResult {
    uint64_t insert_ops = 0;
    uint64_t extract_ops = 0;

    #if defined(PROFILE) && defined(QPID)
    uint64_t min_prio = INT_MAX;
    uint64_t max_prio = 0;
    std::map<int, int> prio_freq;
    #endif
};

struct InputParams {
    // specific to QPID
    int chunk_size = 128; // 'c'
    int n_queues = 128; // 'q'
    int max_batch_size = 128;

    int n_threads = 1; // 't'
    int percent_ins = 50; // 'i'
    int prefill_size = 100000; // 'p'
    int duration_secs = 5; // 'd'
    
    int num_buckets = 5; // 'b'
    int bucket_step = 1; // 'u'
    std::string dist_type = "cubic"; // 'y'

    int delta = 0;
};

// Global atomic flags for ultra-low-overhead synchronization
std::atomic<int> ready_threads{0};
std::atomic<bool> start_signal{false};
std::atomic<bool> stop_signal{false};

// Global baseline tracking the current minimum priority in the queue
std::atomic<uint64_t> global_baseline{1};
std::atomic<int> consecutive_higher_extracts{0}; // Tracks out-of-order "misses"

// A helper function to generate your distribution dynamically
std::discrete_distribution<int> create_workload_distribution(int num_buckets, std::string curve_type) {
    std::vector<double> weights;
    
    for (int i = 0; i < num_buckets; ++i) {
        if (curve_type == "exponential") {
            weights.push_back(std::pow(2.0, i));
        } 
        else if (curve_type == "cubic") {
            weights.push_back(std::pow(i + 1, 3.0));
        }
        else if (curve_type == "flat") { // Good for testing worst-case scenarios!
            weights.push_back(1.0);
        }
    }
    
    return std::discrete_distribution<int>(weights.begin(), weights.end());
}

#ifdef PROFILE
void profile_key(ThreadResult& res, int key) {
    // increment map
    if (res.prio_freq.contains(key)) res.prio_freq[key] += 1;
    else res.prio_freq[key] = 1;
    // update min/max if necessary
    if (key < res.min_prio) res.min_prio = key;
    if (key > res.max_prio) res.max_prio = key;
}
#endif

#ifdef QPID
template <typename PQ_TYPE, typename descriptor>
#else
template <typename PQ_TYPE>
#endif
void prefill(PQ_TYPE& pq,
                int prefill_size,
                int bucket_step,
                int num_buckets,
                std::string dist_type
#ifdef PROFILE
                , ThreadResult& prefill_res
#endif
) {
    // Hardware entropy for non-deterministic prefill
    std::mt19937 prefill_rng(std::random_device{}());
    
    // --- WORKLOAD CONFIGURATION (Must match worker thread) ---
    std::discrete_distribution<int> bucket_dist = create_workload_distribution(num_buckets, dist_type);

    // Read the starting baseline (which should be 1 at the start of the program)
    uint64_t current_base = global_baseline.load(std::memory_order_relaxed);

    #ifdef QPID
    auto* me = new descriptor();
    #endif

    std::cout << "Prefilling " << prefill_size << " elements using quantized skewed buckets..." << std::endl;
    for (int i = 0; i < prefill_size; ++i) {
        // Generate the exact same skewed distribution as the workers
        int bucket_idx = bucket_dist(prefill_rng);
        uint64_t key = current_base + (bucket_idx * bucket_step);

        //pq.push(key, key);
        #ifdef QPID
        me->op_begin();
        #endif
        INSERT_FUNC;
        #ifdef QPID
        me->op_end();
        #endif

        #ifdef PROFILE
        profile_key(std::ref(prefill_res), key);
        #endif
    }
    std::cout << "Prefill complete. Queue is populated and in steady-state." << std::endl;
}

#ifdef QPID
template <typename PQ_TYPE, typename descriptor>
#else
template <typename PQ_TYPE>
#endif
void worker_thread(int tid,
                   int perc_inserts,
                   int num_buckets,
                   int bucket_step,
                   std::string dist_type,
                   int relaxation_tol,
                   PQ_TYPE& pq,
                   ThreadResult& result
#ifdef SPRAYLIST
                 , thread_data_t *data
#endif

) {
    std::mt19937 rng(std::random_device{}() + tid);
    std::uniform_int_distribution<int> op_dist(0, 99);

    // generate workload distribution
    std::discrete_distribution<int> bucket_dist = create_workload_distribution(num_buckets, dist_type);
    if (tid == 0) {
        // print generated distribution probabilities
        std::vector<double> probs = bucket_dist.probabilities();
        std::cout << "Index\tProbability" << std::endl;
        for (size_t i = 0; i < probs.size(); ++i) {
            std::cout << i << "\t" << probs[i] * 100 << std::endl;
        }
        std::cout << "-------------------------" << "\n";
    }

    //*pq.initTID();
    #if defined(QPID)
    auto* me = new descriptor();
    if (tid) pq.init_thread(me, tid);
    else pq.re_init_thread(me);
    #else
    INIT_THREAD;
    #endif

    #ifdef SPRAYLIST
    thread_data_t* data_item = &(data[tid]);
    #endif

    // Local counters to avoid cache-line bouncing during execution
    uint64_t local_inserts = 0;
    uint64_t local_extracts = 0;
    uint64_t local_failed_extracts = 0;

    #if defined(PROFILE) && defined(QPID)
    uint64_t local_min_prio = INT_MAX;
    uint64_t local_max_prio = 0;
    std::map<int,int> local_prio_freqs;
    #endif

    // 2. Signal that this thread is ready
    ready_threads++;

    // 3. Spin-wait until the main thread gives the green light
    while (!start_signal.load(std::memory_order_acquire)) {}

    // 4. Main workload loop
    while (!stop_signal.load(std::memory_order_relaxed)) {
        // Read the current minimum baseline
        //uint64_t current_base = global_baseline.load(std::memory_order_relaxed);
        
        if (op_dist(rng) < perc_inserts) {
            // Generate a skewed bucket index (0 to 4)
            int bucket_idx = bucket_dist(rng);
            // Calculate the actual key to insert
            uint64_t key = (global_baseline.load(std::memory_order_relaxed)) + (bucket_idx * bucket_step);

            #if defined(PROFILE) && defined(QPID)
            // Check if min or max
            if (key < local_min_prio) local_min_prio = key;
            if (key > local_max_prio) local_max_prio = key;
            // Update generated key's frequency
            if (local_prio_freqs.find(key) != local_prio_freqs.end()) local_prio_freqs[key] += 1;
            else local_prio_freqs[key] = 1;
            #endif

            //*pq.push(key, key); // Perform insert

            #ifdef QPID
            me->op_begin();
            #endif
            INSERT_FUNC;
            #ifdef QPID
            me->op_end();
            #endif

            local_inserts++;
        } else {
            //*auto ret = pq.popInternal(); // Perform extract-min
            // #ifdef LINDEN
            // bool completed = false;
            // while (!completed) {
            // #endif


            #ifdef QPID
            me->op_begin();
            #endif
            auto ret = EXTRACT_FUNC;
            #ifdef QPID
            me->op_end();
            #endif

            if (ret) {
                // #ifdef LINDEN
                // completed = true;
                // #endif


                local_extracts++;
                #ifdef MBQ
                uint64_t extracted_key = std::get<0>(ret.value());
                #else
                uint64_t extracted_key = ret.value().first;
                #endif
                
                
                uint64_t current_base = global_baseline.load(std::memory_order_relaxed);
                
                // --- NEW CONSENSUS LOGIC ---
                if (extracted_key <= current_base) {
                    // We found an element at the baseline! The bucket is NOT empty.
                    // This means any recent higher extractions were just relaxed noise.
                    // Reset the consensus counter.
                    if (consecutive_higher_extracts.load(std::memory_order_relaxed) != 0) {
                        consecutive_higher_extracts.store(0, std::memory_order_relaxed);
                    }
                } 
                else {
                    // We extracted a larger element. It might be relaxed noise, 
                    // or the bucket might actually be empty.
                    int misses = consecutive_higher_extracts.fetch_add(1, std::memory_order_relaxed);

                    if (misses > relaxation_tol) {
                        // Consensus reached: We have missed the baseline so many times 
                        // that it must be truly empty. Time to slide the window.
                        uint64_t expected = current_base;
                        uint64_t next_base = current_base + bucket_step;

                        // Only slide forward by exactly ONE bucket_step to maintain distribution shape
                        if (global_baseline.compare_exchange_strong(expected, next_base)) {
                            // We successfully moved the baseline.
                            // Reset the counter so we can start evaluating the new tier.
                            consecutive_higher_extracts.store(0, std::memory_order_relaxed);
                        }
                    }
                }
            }
            // else {
            //     local_failed_extracts++;
            // }
            // #ifdef LINDEN
            // }
            // #endif
        }
    }

    // 5. Save results back after the timer expires
    result.insert_ops = local_inserts;
    result.extract_ops = local_extracts;
    #if defined(PROFILE) && defined(QPID)
    result.max_prio = local_max_prio;
    result.min_prio = local_min_prio;
    result.prio_freq = std::move(local_prio_freqs);
    #endif

    //if (local_failed_extracts) std::cout << "failed extract " << local_failed_extracts << " times.\n";
}

#ifdef QPID
template <typename PQ_TYPE, typename descriptor>
#else
template <typename PQ_TYPE>
#endif
void launch_threads(PQ_TYPE& pq, InputParams& inputs
#ifdef SPRAYLIST
    , thread_data_t* data
#elif defined(PROFILE) && defined(QPID)
    , ThreadResult& prefill_res
#endif
) {
    std::cout << "Launching " << inputs.n_threads << " threads... (Workload: " << inputs.percent_ins << "% Inserts)" << std::endl;

    int relaxation_tol = 5 * inputs.n_threads;

    std::vector<std::thread*> workers;
    std::vector<ThreadResult> results(inputs.n_threads);

    cpu_set_t cpuset;
    for (int i = 0; i < inputs.n_threads; ++i) {
        CPU_ZERO(&cpuset);
        uint64_t coreID = pinning[i];
        CPU_SET(coreID, &cpuset);

        #ifdef QPID
        std::thread *newThread = new std::thread(worker_thread<PQ_TYPE, descriptor>, i, inputs.percent_ins, inputs.num_buckets, inputs.bucket_step, inputs.dist_type, relaxation_tol, std::ref(pq), std::ref(results[i]));
        #elif defined(SPRAYLIST)
        std::thread *newThread = new std::thread(worker_thread<PQ_TYPE>, i, inputs.percent_ins, inputs.num_buckets, inputs.bucket_step, inputs.dist_type, relaxation_tol, std::ref(pq), std::ref(results[i]), data);
        #else
        std::thread *newThread = new std::thread(worker_thread<PQ_TYPE>, i, inputs.percent_ins, inputs.num_buckets, inputs.bucket_step, inputs.dist_type, relaxation_tol, std::ref(pq), std::ref(results[i]));
        #endif
        
        int rc = pthread_setaffinity_np(newThread->native_handle(), sizeof(cpu_set_t), &cpuset);
        if (rc != 0) {
            std::cerr << "Error calling pthread_setaffinity_np: " << rc << "\n";
        }
        workers.push_back(newThread);
    }

    // Wait until all threads are spun up and waiting at the barrier
    while (ready_threads.load() < inputs.n_threads) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    // Give threads a brief moment to settle into their spin-locks
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // --- 3. BENCHMARK RUN PHASE ---
    std::cout << "All threads ready. Starting benchmark for " << inputs.duration_secs << "s..." << std::endl;

    auto start_time = std::chrono::high_resolution_clock::now();
    start_signal.store(true, std::memory_order_release);

    std::this_thread::sleep_for(std::chrono::seconds(inputs.duration_secs));

    stop_signal.store(true, std::memory_order_relaxed);
    auto end_time = std::chrono::high_resolution_clock::now();

    // --- 4. CLEANUP & METRICS PHASE ---
    for (std::thread*& worker : workers) {
        worker->join();
        delete worker;
    }

    std::chrono::duration<double> elapsed = end_time - start_time;
    uint64_t total_inserts = 0;
    uint64_t total_extracts = 0;

    #if defined(PROFILE) && defined(QPID)
    uint64_t min_prio = prefill_res.min_prio;
    uint64_t max_prio = prefill_res.max_prio;
    std::map<int,int> prio_freq;
    prio_freq = std::move(prefill_res.prio_freq);
    #endif

    for (const auto& res : results) {
        total_inserts += res.insert_ops;
        total_extracts += res.extract_ops;
        
        #if defined(PROFILE) && defined(QPID)
        if (res.min_prio < min_prio) min_prio = res.min_prio;
        if (res.max_prio > max_prio) max_prio = res.max_prio;
        for (const auto& [key, value] : res.prio_freq) {
            if (prio_freq.find(key) != prio_freq.end()) prio_freq[key] += value;
            else prio_freq[key] = value;
        }
        #endif
    }

    #if defined(PROFILE) && defined(QPID)
    std::cout << "Frequency of Each Priority:" << std::endl;
    int tot_prios = 0;
    for (const auto& [key, value] : prio_freq) {
        std::cout << "Prio: " << key << ", Freq: " << value << "\n";
        tot_prios++;
    }
    std::cout << "Unique priorities: " << tot_prios << "\n";
    #endif

    uint64_t total_ops = total_inserts + total_extracts;
    double throughput = total_ops / elapsed.count();

    std::cout << "\n--- Benchmark Results ---" << std::endl;
    std::cout << "Actual Duration: " << elapsed.count() << " seconds" << std::endl;
    std::cout << "Total Operations: " << total_ops << " (Inserts: " << total_inserts << ", Extracts: " << total_extracts << ")" << std::endl;
    std::cout << "Throughput: " << std::fixed << std::setprecision(0) << throughput << " ops/sec" << std::endl;
    
    #if defined(PROFILE) && defined(QPID)
    std::cout << "\n";
    std::cout << "Minimum priority: " << min_prio << "\n";
    std::cout << "Maximum priority: " << max_prio << "\n";
    #endif
}

void trial(std::string ds, InputParams& inputs) {
    #if defined(QPID)
        #define _ALG eager_noext_c1_t
        #define _OREC orec_po_t
        #define _CLOCK rdtscp_clock_t

        using descriptor = _ALG<_OREC, clock_policy::_CLOCK>;
        //using OPTSTM = _ALG<_OREC, clock_policy::_CLOCK>;
        using map = skiphash_pq_relaxed<uint32_t, uint32_t, descriptor>;
        auto *me = new descriptor();

        config_t cfg;
        cfg.chunksize = inputs.chunk_size;
        cfg.num_queues = inputs.n_queues;
        cfg.max_batch_size = inputs.max_batch_size;
        cfg.delta = inputs.delta;

        map pq(me, &cfg);
        pq.init_thread(me, 0);
        #if defined(PROFILE) && defined(QPID)
        ThreadResult prefill_res;
        prefill<map, descriptor>(std::ref(pq), inputs.prefill_size, inputs.bucket_step, inputs.num_buckets, inputs.dist_type, std::ref(prefill_res));
        launch_threads<map, descriptor>(std::ref(pq), std::ref(inputs), std::ref(prefill_res));
        #else
        prefill<map, descriptor>(std::ref(pq), inputs.prefill_size, inputs.bucket_step, inputs.num_buckets, inputs.dist_type);
        launch_threads<map, descriptor>(std::ref(pq), std::ref(inputs));
        #endif
    #elif defined(MBQ)
        // Lambda for mapping a priority to a priority level
        auto getBucketID = [&] (uint32_t v) -> mbq::BucketID {
            return v;
        };

        int queue_num = inputs.n_queues;
        int delta = 0;
        int bucketNum = 64;
        int batch1 = inputs.chunk_size;
        int batch2 = inputs.chunk_size;
        int stickiness = 8;

        using MQ_Bucket_Type = mbq::MultiBucketQueue<decltype(getBucketID), decltype(getBucketID), std::greater<mbq::BucketID>, uint32_t, uint32_t>;
        MQ_Bucket_Type pq(getBucketID, getBucketID, inputs.n_queues, inputs.n_threads, delta, bucketNum, batch1, batch2, mbq::increasing, stickiness);
        
        prefill<MQ_Bucket_Type>(std::ref(pq), inputs.prefill_size, inputs.bucket_step, inputs.num_buckets, inputs.dist_type);
        launch_threads<MQ_Bucket_Type>(std::ref(pq), std::ref(inputs));
    #elif defined(PIPQ)
        int heap_list_size = 10000000;
        int cntr_tsh = 10;
        int cntr_max = 100;

        using pipq_t = pq_ns::pipq<uint32_t>;
        auto pq = pipq_t(heap_list_size, inputs.n_threads, cntr_tsh, cntr_max, pinning);
        pq.PQInit();
        pq.init_thread(0);

        prefill<pipq_t>(std::ref(pq), inputs.prefill_size, inputs.bucket_step, inputs.num_buckets, inputs.dist_type);
        launch_threads<pipq_t>(std::ref(pq), std::ref(inputs));
    #elif defined(SPRAYLIST)
        *levelmax = 32;
        thread_data_t *data = (thread_data_t *)malloc(inputs.n_threads * sizeof(thread_data_t));
        for (int i = 0; i < inputs.n_threads; i++) {
            data[i].seed = rand();
            data[i].seed2 = rand();
            data[i].nb_threads = inputs.n_threads;
        }
        seeds = seed_rand();
        sl_intset_t * pq = sl_set_new();
        prefill<sl_intset_t*>(pq, inputs.prefill_size, inputs.bucket_step, inputs.num_buckets, inputs.dist_type);
        launch_threads<sl_intset_t*>(std::ref(pq), std::ref(inputs), data);

        //spawnTasksSpray(sl_spray, graph, source, threadNum, prios, detector_2, data);
    #elif defined(LINDEN)
        int max_offset = 32;
        _init_gc_subsystem();
        pq_t * pq = pq_init(max_offset);
        prefill<pq_t *>(pq, inputs.prefill_size, inputs.bucket_step, inputs.num_buckets, inputs.dist_type);
        launch_threads<pq_t *>(std::ref(pq), std::ref(inputs));
    #elif defined(SMQ)
        const size_t steal_probability = 8; // 1/8 probability of stealing
        if (inputs.chunk_size == 8) {
            const size_t steal_batch_size = 8; // size of batch to steal
            using smq_t = smq_ns::StealingMultiQueue<std::pair<uint32_t,uint32_t>,uint32_t,steal_probability,steal_batch_size,true>;
            auto pq = smq_t(inputs.n_threads);
            prefill<smq_t>(pq, inputs.prefill_size, inputs.bucket_step, inputs.num_buckets, inputs.dist_type);
            launch_threads<smq_t>(std::ref(pq), std::ref(inputs));
        } else if (inputs.chunk_size == 32) {
            const size_t steal_batch_size = 32; // size of batch to steal
            using smq_t = smq_ns::StealingMultiQueue<std::pair<uint32_t,uint32_t>,uint32_t,steal_probability,steal_batch_size,true>;
            auto pq = smq_t(inputs.n_threads);
            prefill<smq_t>(pq, inputs.prefill_size, inputs.bucket_step, inputs.num_buckets, inputs.dist_type);
            launch_threads<smq_t>(std::ref(pq), std::ref(inputs));
        } else if (inputs.chunk_size == 128) {
            const size_t steal_batch_size = 128; // size of batch to steal
            using smq_t = smq_ns::StealingMultiQueue<std::pair<uint32_t,uint32_t>,uint32_t,steal_probability,steal_batch_size,true>;
            auto pq = smq_t(inputs.n_threads);
            prefill<smq_t>(pq, inputs.prefill_size, inputs.bucket_step, inputs.num_buckets, inputs.dist_type);
            launch_threads<smq_t>(std::ref(pq), std::ref(inputs));
        }
    #else
        std::cout << "Invalid data structure passed: " << ds << ", exiting.\n";
    #endif
}

int main(int argc, char** argv) {
    // default values
    struct InputParams inputs;

    // 2. Parse command-line arguments using getopt
    int opt;
    while ((opt = getopt(argc, argv, "t:i:p:d:c:q:b:u:y:h")) != -1) {
        switch (opt) {
            case 't':
                inputs.n_threads = std::stoi(optarg);
                break;
            case 'i':
                inputs.percent_ins = std::stoi(optarg);
                break;
            case 'p':
                inputs.prefill_size = std::stoi(optarg);
                break;
            case 'd':
                inputs.duration_secs = std::stoi(optarg);
                break;
            case 'c':
                inputs.chunk_size = std::stoi(optarg);
                inputs.max_batch_size = inputs.chunk_size;
                break;
            case 'q':
                inputs.n_queues = std::stoi(optarg);
                break;
            case 'b':
                inputs.num_buckets = std::stoi(optarg);
                break;
            case 'u':
                inputs.bucket_step = std::stoi(optarg);
                break;
            case 'y':
                inputs.dist_type = optarg;
                if (!(inputs.dist_type == "cubic" || inputs.dist_type == "exponential" || inputs.dist_type == "flat")) {
                    std::cout << "Invalid distribution type passed (" << optarg << "), exiting.\n";
                    return 0;
                }
                break;
            case 'h': // Help menu
            default:
                std::cout << "Usage: " << argv[0] << " [OPTIONS]\n"
                          << "Options:\n"
                          << "  -t <int>  Number of worker threads (default: " << inputs.n_threads << ")\n"
                          << "  -i <int>  Percentage of operations that are INSERTS (default: " << inputs.percent_ins << "%)\n"
                          << "  -p <int>  Number of elements to prefill (default: " << inputs.prefill_size << ")\n"
                          << "  -d <int>  Benchmark duration in seconds (default: " << inputs.duration_secs << "s)\n"
                          << "  -c <int>  Chunk size (for QPID) (default: " << inputs.chunk_size << ")\n"
                          << "  -q <int>  Num Queues (for QPID,MBQ) (default: " << inputs.n_queues << ")\n"
                          << "  -b <int>  Num Buckets (default: " << inputs.num_buckets << ")\n"
                          << "  -u <int>  Bucket Step (default: " << inputs.bucket_step << ")\n"
                          << "  -y <str>  Workload distrbution ['cubic', ''] (default: " << inputs.dist_type << ")\n"
                          << "  -h        Show this help message\n";
                return 0;
        }
    }

    #if defined(PROFILE) && defined(QPID)
    std::cout << "\n\nWARNING: You are running in profiling mode... turn off if recording results.\n\n";
    #endif

    #ifdef QPID
    std::string ds = "QPID";
    #elif defined(MBQ)
    std::string ds = "MBQ";
    #elif defined(PIPQ)
    std::string ds = "PIPQ";
    #elif defined(LINDEN)
    std::string ds = "LINDEN";
    #elif defined(SMQ)
    std::string ds = "SMQ";
    #else
    std::string ds = "TBD";
    #endif

    // 3. Print the configuration to verify before launching
    std::cout << "========================================\n"
              << "Running Benchmark with Configuration:\n"
              << "  Data Structure:   " << ds << "\n";

    if (ds == "QPID" || ds == "MBQ" || ds == "SMQ")
        std::cout << "  Num queues:       " << inputs.n_queues << "\n";
    if (ds == "QPID")
        std::cout << "  Chunk size:       " << inputs.chunk_size << "\n";
    
    std::cout << "  \n"
              << "  Threads:          " << inputs.n_threads << "\n"
              << "  Workload:         " << inputs.percent_ins << "% Inserts / " << (100 - inputs.percent_ins) << "% Extracts\n"
              << "  Duration:         " << inputs.duration_secs << " seconds\n"
              << "  Prefill Size:     " << inputs.prefill_size << "\n"
              << "  \n"
              << "  Distribution:     " << inputs.dist_type << "\n"
              << "  Num Buckets:      " << inputs.num_buckets << "\n"
              << "  Bucket Step       " << inputs.bucket_step << "\n"
              << "========================================\n";

    trial(ds, std::ref(inputs));

    return 0;
}
