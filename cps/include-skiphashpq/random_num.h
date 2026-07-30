#include <iostream>
#include <cstdint>
#include <thread>
#include <vector>
#include <random>
#include <mutex>
#include <cassert>
#include <limits>

class random_num {
public:

    // --- C++ Engine Requirements ---
    using result_type = uint64_t;
    static constexpr result_type min() { return std::numeric_limits<result_type>::min(); }
    static constexpr result_type max() { return std::numeric_limits<result_type>::max(); }
    
    // operator() is the standard way to get a number from an engine
    result_type operator()() {
        return rng();
    }

    inline static thread_local uint64_t state[2];
    inline static thread_local bool seeded = false;

    struct MasterSeeder {
        std::mutex mtx;
        std::mt19937_64 engine;
        MasterSeeder() : engine(std::random_device{}()) {}
    };
    inline static MasterSeeder seeder;

    // Seeding function that initializes a single thread's state.
    static void seed_this_thread() {
        std::lock_guard<std::mutex> lock(seeder.mtx);
        state[0] = seeder.engine();
        state[1] = seeder.engine();
        seeded = true;
    }

    static inline uint64_t rotl(const uint64_t x, int k) {
        return (x << k) | (x >> (64 - k));
    }

    uint64_t rng() {
        // Lazy, on-demand seeding
        if (!seeded) {
            seed_this_thread();
        }
        const uint64_t s0 = state[0];
        uint64_t s1 = state[1];
        const uint64_t result = s0 + s1;
        s1 ^= s0;
        state[0] = rotl(s0, 24) ^ s1 ^ (s1 << 16);
        state[1] = rotl(s1, 37);
        return result;
    }
};