#include <atomic>
#include <vector>
#include <thread>
#include <iostream>

// Defined to prevent False Sharing (64 bytes is standard for x86 cache lines)
constexpr size_t CACHE_LINE_SIZE = 64;

struct alignas(CACHE_LINE_SIZE) thread_local_counts {
    // We use plain longs here because only ONE thread (the owner) 
    // ever writes to these. No atomic overhead needed on the hot path.
    long long produced = 0;
    long long consumed = 0;
};

class termination_detector {
    // Array of padded counters (one per thread)
    std::vector<thread_local_counts> local_counters;
    
    // Global atomic sums
    std::atomic<long long> global_produced{0};
    std::atomic<long long> global_consumed{0};

public:
    termination_detector(int n_threads) : local_counters(n_threads) {}

    // ----------------------------------------------------------------
    // HOT PATH: Called frequently inside the tight loop
    // ----------------------------------------------------------------
    
    // Call this immediately before pushing a new task to the queue
    inline void notify_produced(int tid) {
        local_counters[tid].produced++;
    }

    // Call this immediately after successfully popping a task
    inline void notify_consumed(int tid) {
        local_counters[tid].consumed++;
    }

    // ----------------------------------------------------------------
    // COLD PATH: Called ONLY when the queue appears empty
    // ----------------------------------------------------------------
    
    bool try_terminate(int tid) {
        // 1. Snapshot local values
        long long p_count = local_counters[tid].produced;
        long long c_count = local_counters[tid].consumed;

        // 2. RESET local values immediately 
        // We do this now so any work produced *while* we are waiting 
        // in this function starts from 0 in the next cycle.
        local_counters[tid].produced = 0;
        local_counters[tid].consumed = 0;

        // 3. Update Global PRODUCED first (CRITICAL ORDERING)
        // We must make "work created" visible before "work finished".
        if (p_count > 0) {
            global_produced.fetch_add(p_count, std::memory_order_release);
        }

        // 4. Memory Fence
        // Prevents the compiler/CPU from reordering the consumed update before the produced update.
        std::atomic_thread_fence(std::memory_order_seq_cst);

        // 5. Update Global CONSUMED second
        if (c_count > 0) {
            global_consumed.fetch_add(c_count, std::memory_order_release);
        }

        // 6. Check for Global Termination
        // We use acquire to ensure we see the latest writes from other threads.
        long long p_total = global_produced.load(std::memory_order_acquire);
        long long c_total = global_consumed.load(std::memory_order_acquire);

        //std::cout << "p_total: " << p_total << ", c_total " << c_total << "\n";

        return p_total == c_total;
    }
};