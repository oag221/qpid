// #ifndef RBP && not defined(SC)
// #include "include-skiphashpq/skiphash_pq_relaxed.h"
// #else

//#include "include-skiphashpq/skiphash_pq_relaxed.h"
#include "skiphash_pq_relaxed.h"

// #endif
// Object to track the range of priorities inserted
class prio_tracker {
    struct range_delta_key {
        uint64_t count;
        uint64_t smallest = INT_MAX;
        uint64_t largest = 0;
    };

    std::vector<std::unordered_map<int,int>> prios;

    const int DELTA;

  public:
    prio_tracker(int num_threads, int delta) : DELTA(delta) {
        prios.resize(num_threads);
    }

    // called by each thread
    void append_prio(int tid, int prio) {
        if (prios[tid].find(prio) != prios[tid].end()) {
            prios[tid][prio] += 1;
        } else {
            // first time inserting this prio
            prios[tid][prio] = 1;
        }
    }

    void print_sum_prios() {
        // sum into a single map
        std::unordered_map<int,range_delta_key> summed_prios;
        for (int i = 0; i < prios.size(); i++) {
            for (const auto& [key, value] : prios[i]) {
              #ifdef LOG_BUCKETS
                uint64_t delta_key = skiphash_pq_relaxed_base::get_fine_grained_bucket(key);
              #else
                uint64_t delta_key = key >> DELTA;
              #endif

                auto& entry = summed_prios[delta_key];
                entry.count += value;

                if (key < entry.smallest) entry.smallest = key;
                if (key > entry.largest)  entry.largest = key;
            }
        }

        // copy to vector, to easily sort
        std::vector<std::pair<int,range_delta_key>> sorted_prios;
        sorted_prios.reserve(summed_prios.size());
        for (const auto& entry : summed_prios) {
            sorted_prios.push_back(entry);
        }
        // sort the vector
        std::sort(sorted_prios.begin(), sorted_prios.end(), 
            [](const auto& a, const auto& b) {
                return a.first < b.first;
            }
        );

        // print sorted_prios
        long tot_elems = 0;
        int mod_ = 5, cnt = 0;
        for (const auto& [key, value] : sorted_prios) {
            tot_elems += value.count;
            std::cout << "[" << key << "]: " << value.count << ", range = " << (value.largest - value.smallest + 1) << " [" << value.smallest << " - " << value.largest <<  "]" << std::endl;
            if (++cnt % mod_ == 0) std::cout << "\t (value count = " << tot_elems << ")\n";
        }

        std::cout << "Total number of elements: " << tot_elems << "\n";
        std::cout << "Total unique priorities: " << sorted_prios.size() << "\n";
    }

};
