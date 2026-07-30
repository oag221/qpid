#include <atomic>
#include <vector>
#include <thread>
#include <iostream>

class termination_detector_2 {
    // Tracks number of idle threads
    std::atomic<int> num_idle{0};

    // Number of threads
    const int THREADS;

public:
    termination_detector_2(int n_threads) : THREADS(n_threads) {}

    int increment_idle() {
        return num_idle.fetch_add(1, std::memory_order_acq_rel) + 1;
    }

    void decrement_idle() {
        num_idle.fetch_sub(1, std::memory_order_acq_rel);
    }

    int get_idle() {
        return num_idle.load(std::memory_order_relaxed);
    }

    bool terminate(int num=-1) {
        if (num == -1) num = get_idle();
        return (num >= THREADS);
    }
};