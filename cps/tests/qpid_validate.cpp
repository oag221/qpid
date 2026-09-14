// qpid_validate.cpp -- concurrency correctness checker for skiphash_pq_relaxed (QPID).
//
// Runs a mixed insert / extract-min workload, then drains the queue, and checks
// that no job was lost or duplicated:
//
//     inserted_count == extracted_count + count_left_in_structure
//     inserted_keysum == extracted_keysum + keysum_left_in_structure
//
// The drain phase matters: extract_min() hands a thread a whole private chunk,
// and only that thread can consume it, so every thread must extract until it
// sees an empty queue before the totals are meaningful.
//
// This is a stress test, not a proof.  It is nonetheless what caught both of the
// subtle bugs in the atomic-claim work: a chunk retired while an enqueuer was
// still appending to it, and an over-claimed slot index skipping a live job.
//
// Build (note -std=c++20 and -fpermissive, which the STM headers require):
//   g++ -std=c++20 -O3 -march=native -fpermissive -I<repo>/cps \
//       -o qpid_validate qpid_validate.cpp -lpthread
//
// Or just use run_qpid_validate.sh, which builds it and runs the whole matrix.

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <random>
#include <thread>
#include <vector>

#include <include-skiphashpq/optstm2/eager_noext_c1.h>
#include <include-skiphashpq/skiphash_pq_relaxed.h>

OPTSTM2_GLOBALS_INITIALIZER;

/// Minimal stand-in for the benchmark's config_t
struct config_t {
  bool order = 0;
  uint8_t max_levels = 32;
  uint16_t threads = 1;
  uint32_t buckets = 1048576;
  uint32_t chunksize = 128;
  uint32_t num_queues = 128;
  uint32_t max_batch_size = 128;
  uint32_t delta = 0;
  uint32_t pool_reserve = 1000;
  uint32_t pool_init_chunks = 1000;
};

using descriptor = eager_noext_c1_t<orec_po_t, clock_policy::rdtscp_clock_t>;
using PQ = skiphash_pq_relaxed<uint32_t, uint32_t, descriptor>;

// NUMA-by-NUMA pinning, matching the benchmarks
static const int PIN[96] = {
    0,  2,  4,  6,  8,  10, 12, 14, 16, 18, 20, 22, 24, 26, 28, 30,
    32, 34, 36, 38, 40, 42, 44, 46, 1,  3,  5,  7,  9,  11, 13, 15,
    17, 19, 21, 23, 25, 27, 29, 31, 33, 35, 37, 39, 41, 43, 45, 47,
    48, 50, 52, 54, 56, 58, 60, 62, 64, 66, 68, 70, 72, 74, 76, 78,
    80, 82, 84, 86, 88, 90, 92, 94, 49, 51, 53, 55, 57, 59, 61, 63,
    65, 67, 69, 71, 73, 75, 77, 79, 81, 83, 85, 87, 89, 91, 93, 95};

static std::atomic<int> g_ready{0};
static std::atomic<bool> g_go{false}, g_stop{false};

struct alignas(128) Acc {
  unsigned long long ins_sum = 0, ext_sum = 0;
  unsigned long ins_n = 0, ext_n = 0;
};

static void usage(const char *argv0) {
  std::printf(
      "usage: %s [threads] [seconds] [priorities] [num_queues] [chunk] [strict]\n"
      "  threads     worker threads                       (default 48)\n"
      "  seconds     duration of the mixed phase          (default 3)\n"
      "  priorities  distinct priorities in play          (default 15)\n"
      "  num_queues  lanes per priority bucket (q)        (default 128)\n"
      "  chunk       jobs per chunk (c)                   (default 128)\n"
      "  strict      1 = extract_min_strict, 0 = extract_min (default 0)\n"
      "\nexit status: 0 if the queue conserved every job, 1 otherwise\n",
      argv0);
}

int main(int argc, char **argv) {
  if (argc > 1 && (!std::strcmp(argv[1], "-h") || !std::strcmp(argv[1], "--help"))) {
    usage(argv[0]);
    return 0;
  }
  int nthreads = argc > 1 ? std::atoi(argv[1]) : 48;
  int secs     = argc > 2 ? std::atoi(argv[2]) : 3;
  int nprios   = argc > 3 ? std::atoi(argv[3]) : 15;
  int q        = argc > 4 ? std::atoi(argv[4]) : 128;
  int c        = argc > 5 ? std::atoi(argv[5]) : 128;
  int strict   = argc > 6 ? std::atoi(argv[6]) : 0;

  if (nthreads < 1 || secs < 1 || nprios < 1 || q < 1 || c < 1) {
    usage(argv[0]);
    return 2;
  }

  config_t cfg;
  cfg.threads = nthreads;
  cfg.num_queues = q;
  cfg.chunksize = c;
  cfg.max_batch_size = c;

  auto *me0 = new descriptor();
  PQ pq(me0, &cfg);
  pq.init_thread(me0, 0);

  std::vector<Acc> acc(nthreads);
  std::vector<std::thread *> th;
  th.reserve(nthreads);

  for (int i = 0; i < nthreads; i++) {
    auto *t = new std::thread([&, i] {
      auto *me = new descriptor();
      pq.init_thread(me, i);
      std::mt19937 rng(12345u + i);
      std::uniform_int_distribution<int> op(0, 99);
      std::uniform_int_distribution<uint32_t> kd(1, nprios);
      Acc &a = acc[i];

      g_ready++;
      while (!g_go.load(std::memory_order_acquire)) {}

      // Phase 1: mixed inserts and extracts
      while (!g_stop.load(std::memory_order_relaxed)) {
        if (op(rng) < 50) {
          uint32_t k = kd(rng);
          me->op_begin();
          pq.insert(me, k, k);
          me->op_end();
          a.ins_sum += k;
          a.ins_n++;
        } else {
          me->op_begin();
          auto r = strict ? pq.extract_min_strict(me) : pq.extract_min(me);
          me->op_end();
          if (r) {
            a.ext_sum += r->first;
            a.ext_n++;
          }
        }
      }

      // Phase 2: drain.  Each thread must run this itself -- a thread's private
      // chunk (extract_min) is consumable only by its owner.
      while (true) {
        me->op_begin();
        auto r = strict ? pq.extract_min_strict(me) : pq.extract_min(me);
        me->op_end();
        if (!r) break;
        a.ext_sum += r->first;
        a.ext_n++;
      }
    });
    cpu_set_t cs;
    CPU_ZERO(&cs);
    CPU_SET(PIN[i % 96], &cs);
    pthread_setaffinity_np(t->native_handle(), sizeof(cs), &cs);
    th.push_back(t);
  }

  while (g_ready.load() < nthreads)
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  g_go.store(true, std::memory_order_release);
  std::this_thread::sleep_for(std::chrono::seconds(secs));
  g_stop.store(true, std::memory_order_relaxed);
  for (auto *t : th) {
    t->join();
    delete t;
  }

  unsigned long long is = 0, es = 0;
  unsigned long in = 0, en = 0;
  for (auto &a : acc) {
    is += a.ins_sum;
    es += a.ext_sum;
    in += a.ins_n;
    en += a.ext_n;
  }
  me0->op_begin();
  auto d = pq.dump(me0, false);
  me0->op_end();

  std::printf("threads=%-3d %s q=%-4d c=%-4d prios=%d\n", nthreads,
              strict ? "STRICT" : "batch ", q, c, nprios);
  std::printf("  inserted : n=%-12lu sum=%llu\n", in, is);
  std::printf("  extracted: n=%-12lu sum=%llu\n", en, es);
  std::printf("  left in structure: n=%ld sum=%ld\n", d.first, d.second);

  bool ok = (in == en + (unsigned long)d.first) &&
            (is == es + (unsigned long long)d.second);
  if (!ok) {
    long dn = (long)in - (long)(en + (unsigned long)d.first);
    std::printf("  ==> *** FAIL: %ld job(s) %s ***\n", dn < 0 ? -dn : dn,
                dn > 0 ? "lost" : "duplicated");
  } else {
    std::printf("  ==> PASS\n");
  }
  return ok ? 0 : 1;
}
