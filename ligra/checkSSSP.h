#include <limits>
#include <atomic>
#include "ligra.h"
#include "index_map.h"

template <class vertex>
int checkSSSP(graph<vertex>& G, uintE start, array_imap<uintE> const &dists) {
  size_t n = G.n;
  uint64_t w = 0;
  std::atomic<bool> failed{false};
  for(uint64_t v = 0; v < n; v++) {
    uintE d = dists[v];
    if (d == std::numeric_limits<uintE>::max())
      continue;

    for (uint64_t i = 0; i < G.V[v].getOutDegree(); i++) {
      uintE ngh = G.V[v].getOutNeighbor(i);
      uintE nghDist = dists[ngh];
      uintE weight = G.V[v].getOutWeight(i);
      if (nghDist > d + weight) {
        failed = true;
        w++;
        // std::cout << "wrong dist on (" << v << ", " << ngh << ")\n";
      }
    }
  };

  std::atomic<uintE> max{0};
  for(uint64_t v = 0; v < n; v++) {
    uintE d = dists[v];
    if (d == UINT32_MAX)
      continue;

    uintE m = max.load(std::memory_order_relaxed);
    while (d > m) {
      bool success = max.compare_exchange_weak(
        m, d, 
        memory_order_release, memory_order_relaxed
      );
      if (success) break;
    }
  };

  std::cout << "max dist = " << max.load() << "\n";

  if (failed) {
    std::cout << "verification failed\n";
    std::cout << w << " wrong dists\n";
    return 1;
  }
  std::cout << "verification success\n";
  return 0;
}

template <class vertex>
int checkBFS(graph<vertex>& G, uintE start, array_imap<uintE> const &dists) {
  size_t n = G.n;
  uint64_t w = 0;
  std::atomic<bool> failed{false};
  for(uint64_t v = 0; v < n; v++) {
    uintE d = dists[v];
    if (d == std::numeric_limits<uintE>::max())
      continue;

    for (uint64_t i = 0; i < G.V[v].getOutDegree(); i++) {
      uintE ngh = G.V[v].getOutNeighbor(i);
      uintE nghDist = dists[ngh];
      uintE weight = 1;
      if (nghDist > d + weight) {
        failed = true;
        w++;
        // std::cout << "wrong dist on (" << v << ", " << ngh << ")\n";
      }
    }
  };

  std::atomic<uintE> max{0};
  for(uint64_t v = 0; v < n; v++) {
    uintE d = dists[v];
    if (d == UINT32_MAX)
      continue;

    uintE m = max.load(std::memory_order_relaxed);
    while (d > m) {
      bool success = max.compare_exchange_weak(
        m, d, 
        memory_order_release, memory_order_relaxed
      );
      if (success) break;
    }
  };

  std::cout << "max dist = " << max.load() << "\n";

  if (failed) {
    std::cout << "verification failed\n";
    std::cout << w << " wrong dists\n";
    return 1;
  }
  std::cout << "verification success\n";
  return 0;
}