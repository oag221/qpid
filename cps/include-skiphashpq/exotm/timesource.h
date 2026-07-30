/// timesource.h provides RDTSCP and GV1 clocks, for assigning start and commit
/// times to transactions.

#pragma once

#include <atomic>
#include <climits>
#include <cstdint>
#include <x86intrin.h>

namespace clock_policy {

/// A special value that is larger than any value that any of the clock
/// algorithms will return, and that won't be mistaken for a pointer.
static const uint64_t END_OF_TIME = ULLONG_MAX;

/// An implementation of a clock using rdtscp
/// + No scalability bottlenecks
/// + No shared state
/// - Kind of high latency on each operation start
/// - Extra fencing concerns on begin()
/// - Does not support "skip validation" for infrequent writers
struct rdtscp_clock_t {
  typedef std::atomic<uint64_t> time_snapshot_t;

  /// Global state and methods for the rdtscp_clock_t
  struct global_t {};

  /// Local (per-thread) state and methods for the rdtscp_clock_t
  struct local_t {

    /// Use rdtscp to get the hardware clock cycle count
    uint64_t get_time(global_t &) {
      unsigned int dummy;
      return __rdtscp(&dummy);
    }

    /// Use rdtscp to get the hardware clock cycle count, but enforces strong
    /// ordering with an atomic add
    uint64_t get_time_strong_ordering(global_t &g) { return get_time(g); }

    /// Not needed for RdtscpTimesource. Instead we do the same thing as
    /// get_time()
    uint64_t increment_get(global_t &g) { return get_time_strong_ordering(g); }

    /// No-op for RdtscpTimesource
    void increment(global_t &) {}

    /// Ensure the clock value is not smaller than the provided `val`
    ///
    /// @param g    A reference to the clock's global state
    /// @param val  The minimum acceptable value for the clock
    void ensure_min(global_t &, uint64_t) {}

    /// Event handler for when an orec issue causes an operation to abort
    void on_abort(uint64_t) {}

    /// Event handler for when an operation finishes
    void on_end(global_t &) {}

#if 0
  /// Use rdtsc to get the hardware clock cycle count without ordering
  static uintptr_t get_time_relaxed() { return __rdtsc(); }
#endif
  };

  /// This clock advances without the user calling advance()
  static constexpr bool LOGICAL = false;
};

/// An implementation of a clock using a global shared counter
/// + Low latency on each operation start
/// + No extra fencing concerns on begin()
/// + Supports "skip validation" for infrequent writers
/// - High risk of scalability problems if there are lots of small writers
struct gv1_clock_t {
  typedef uint64_t time_snapshot_t;

  /// Global state and methods for the gv1_clock_t
  struct global_t {
    /// A global shared (atomic) counter, aligned to a cache line
    alignas(128) std::atomic<uint64_t> counter;

    /// Construct the gv1 clock with a value of 1
    global_t() : counter(1) {}
  };

  /// Local (per-thread) state and methods for the gv1_clock_t
  struct local_t {
    /// Report the current time
    uint64_t get_time(global_t &g) { return g.counter; }

    /// get_time_strong_ordering is the same for CounterTimesource
    uint64_t get_time_strong_ordering(global_t &g) { return g.counter; }

    /// Runs the orec_t.h implementation of increment_get()
    uint64_t increment_get(global_t &g) { return 1 + g.counter.fetch_add(1); }

    /// Increment the clock, and ignore the new value.  This is useful when
    /// doing abort-time bumping in undo-based STM.
    void increment(global_t &g) { g.counter++; }

    /// Ensure the clock value is not smaller than the provided `val`
    ///
    /// @param g    A reference to the clock's global state
    /// @param val  The minimum acceptable value for the clock
    void ensure_min(global_t &g, uint64_t val) {
      uintptr_t ts = get_time_strong_ordering(g);
      if (val > ts)
        increment(g);
    }

    /// Event handler for when an orec issue causes an operation to abort
    void on_abort(uint64_t) {}

    /// Event handler for when an operation finishes
    void on_end(global_t &) {}
  };

  /// This clock only advances when the user calls advance()
  static constexpr bool LOGICAL = true;
};

/// An implementation of a clock using the "gv5" global shared counter algorithm
/// + Low latency on each operation start
/// + No extra fencing concerns on begin()
/// + Supports "skip validation" for infrequent writers
/// + Avoids scalability bottleneck by avoiding increments at commit time
/// - Requires extra logic on aborts, because the clock might be out of date
struct gv5_clock_t {
  typedef uint64_t time_snapshot_t;

  /// Global state and methods for the gv5_clock_t
  struct global_t {
    /// A global shared (atomic) counter, aligned to a cache line
    alignas(128) std::atomic<uint64_t> counter;

    /// Construct the gv1 clock with a value of 1
    global_t() : counter(1) {}
  };

  /// Local (per-thread) state and methods for the gv1_clock_t
  struct local_t {
    /// When the user of local_t last aborted, this is the orec value that
    /// caused the abort
    uint64_t last_abort = END_OF_TIME;

    /// Report the current time
    uint64_t get_time(global_t &g) {
      uint64_t res = g.counter;
      // Usually this is a no-op, but in STM with timestamp extension, it
      // matters.
      on_end(g);
      return res;
    }

    /// get_time_strong_ordering is the same for CounterTimesource
    uint64_t get_time_strong_ordering(global_t &g) { return get_time(g); }

    /// Runs the orec_t.h implementation of increment_get()
    uint64_t increment_get(global_t &g) { return 1 + g.counter; }

    /// Increment the clock, and ignore the new value.  This is useful when
    /// doing abort-time bumping in undo-based STM.
    void increment(global_t &) {}

    /// Ensure the clock value is not smaller than the provided `val`
    ///
    /// @param g    A reference to the clock's global state
    /// @param val  The minimum acceptable value for the clock
    void ensure_min(global_t &, uint64_t) {}

    /// Event handler for when an orec issue causes an operation to abort
    void on_abort(uint64_t abort_time) { last_abort = abort_time; }

    /// Event handler for when an operation finishes
    void on_end(global_t &g) {
      if (last_abort != END_OF_TIME) {
        uint64_t time = g.counter;
        while (time < last_abort)
          g.counter.compare_exchange_strong(time, last_abort);
      }
      last_abort = END_OF_TIME;
    }
  };

  /// This clock only advances when the user calls advance()
  static constexpr bool LOGICAL = true;
};

// NB:  The `gv4` clock is probably only of interest on systems that lack a
//      dedicated fetch-add instruction.  Since we are focused on x86, gv4
//      probably isn't interesting.

// NB:  It seems that the `dctl` clock might not really be a different clock, as
//      much as a different optSTM implementation built atop the gv5 clock?
} // namespace clock_policy