// Needs Review

#pragma once

#include <atomic>
#include <climits>
#include <x86intrin.h>

#include "../include/minivector.h"
#include "timesource.h"

#define likely(x) __builtin_expect(!!(x), 1)
#define unlikely(x) __builtin_expect(!!(x), 0)

/// exotm_t encapsulates all of the state and functionality needed by a thread
/// that uses the exoTM transactional mechanisms. This includes per-thread state
/// and the global clock.
///
/// The exotm_t class can be thought of as a "reusable descriptor".  This is
/// important, because it means we can tolerate a small amount of dynamic memory
/// allocation within its implementation.
///
/// exoTM is just a mechanism.  It supports reading orecs and safely writing
/// them.  It does not deal with program data, validation, safe memory
/// reclamation, etc.  Policies built on top of exoTM need to handle all of
/// that.
///
/// exotm_t does not specify where its orecs live.  A policy may choose to
/// maintain a table of orecs, or to place orecs in objects.  It should be
/// possible for programmers to associate multiple orecs with different parts of
/// an object.
///
/// @tparam CLOCK The clock to use with this exoTM instance
template <class CLOCK> class exotm_t {
  using CLOCK_T = CLOCK;                       // A name for the clock type
  static const uint64_t LOCK_BIT = 1ULL << 63; // MSB is the lock bit for orecs
  static typename CLOCK_T::global_t global_clock; // The global clock

public:
  /// A special value that is larger than any value that CLOCK_T will return,
  /// and that won't be mistaken for a pointer.
  static const uint64_t END_OF_TIME = clock_policy::END_OF_TIME;

  /// For communicating how a policy wants orecs released during an unwind
  enum UNWIND_TYPES { ROLLBACK_ORECS, BUMP_ORECS };

  /// The ownership record type
  ///
  /// NB: In order for exoTM to protect its mechanisms while still letting
  ///     policies embed orecs in objects, orec_t is public, its constructor is
  ///     public, and its fields are private.
  class orec_t {
    friend exotm_t;

    std::atomic<uintptr_t> curr; // The current value of the orec
    uintptr_t prev;              // Prior version of `curr`, for easy rollback
  public:
    /// Default construct an orec as unheld with version 0
    orec_t() : curr(0), prev(0) {}
  };

private:
  // TODO: Use CLOCK_T::time_snapshot_t?
  std::atomic<uint64_t> start_time; // This operation's start time, or EOT
  minivector<orec_t *> locks;       // All orecs held by the current transaction
  const uint64_t my_lock;           // This thread's unique lock word
  uint64_t last_wo_end_time = 0;    // Time of last wo_end
  bool unwound = false;             // Are we between unwind() and wo_end()?
  typename CLOCK_T::local_t clock;  // For accessing the clock

public:
  /// Construct a thread's exoTM context
  exotm_t()
      : start_time(END_OF_TIME),
        my_lock(LOCK_BIT | reinterpret_cast<uintptr_t>(this)) {}

  /// Start using exoTM to read orecs
  void ro_begin() {
    // Read the clock, with sufficient (platform-defined) fencing to ensure that
    // all orecs will be read *after* this clock read.
    //
    // TODO:  Investigate coupling this clock with the SMR's clock
    //
    // TODO:  Should the clock tell us if we need a fence or not?
    uint64_t time = clock.get_time_strong_ordering(global_clock);
    start_time.exchange(time);
  }

  /// Stop using exoTM to read orecs
  void ro_end() {
    // TODO:  Investigate if this store could be skipped, to save a fence
    start_time = END_OF_TIME;
    clock.on_end(global_clock);
  }

  /// Ensure that `orec`'s time is <= `this.start_time` and `orec` isn't locked.
  ///
  /// @param orec The orec to check
  ///
  /// @return If the orec is too new or locked by someone other than the caller,
  ///         then END_OF_TIME.  Otherwise, the observed value of the orec will
  ///         be returned.
  uint64_t check_orec(const orec_t *orec) {
    // NB: this is a seqlock read acquire... can't be relaxed
    auto res = orec->curr.load(std::memory_order_acquire);
    if (res <= start_time || res == my_lock)
      return res;
    if (!is_locked(res))
      clock.on_abort(res);
    return END_OF_TIME;
  }

  /// Ensure that `orec`'s value is still `val`
  ///
  /// @param orec The orec to check
  /// @param val  The expected value of the orec
  ///
  /// @return true if the orec value equals val, false otherwise
  bool check_continuation(const orec_t *orec, uint64_t val) {
    // NB: this is a seqlock read acquire... can't be relaxed
    auto res = orec->curr.load(std::memory_order_acquire);
    if (res <= val)
      return true;
    if (!is_locked(res))
      clock.on_abort(res);
    return false;
  }

  /// A specialization of `check_orec` that also returns the lock state
  ///
  /// @param orec   The orec to check
  /// @param locked A ref param to indicate if the location is locked
  ///
  /// @return If the orec is too new or locked by someone other than the caller,
  ///         then END_OF_TIME.  Otherwise, the observed value of the orec will
  ///         be returned.
  uint64_t check_orec(const orec_t *orec, bool &locked) {
    // NB: this is a seqlock read acquire... can't be relaxed
    auto res = orec->curr.load(std::memory_order_acquire);
    locked = is_locked(res);
    if (res <= start_time || res == my_lock)
      return res;
    if (!locked)
      clock.on_abort(res);
    return END_OF_TIME;
  }

  /// A specialization of `check_continuation` that also returns if the caller
  /// owns the orec.
  ///
  /// @param orec The orec to check
  /// @param val  The expected value of the orec
  /// @param mine A ref param to report if the caller is the owner
  ///
  /// @return true if the orec value equals val, false otherwise
  bool check_continuation(const orec_t *orec, uint64_t val, bool &mine) {
    // NB: this is a seqlock read acquire... can't be relaxed
    auto res = orec->curr.load(std::memory_order_acquire);
    mine = res == my_lock;
    if (res <= val)
      return true;
    if (!is_locked(res))
      clock.on_abort(res);
    return false;
  }

  /// Start using exoTM to read and write orecs
  void wo_begin() {
    // Read the hardware clock, just like in ro_begin()
    uint64_t time =
        clock.get_time_strong_ordering(global_clock); // TODO: get_time_relaxed?
    start_time.exchange(time);
    // Mark that we're not unwinding
    //
    // NB: we set it here so hopefully the compiler can propagate it
    unwound = false;
  }

  /// Acquire an orec, only if its version is consistent with `this.start_time`
  ///
  /// @param orec The orec to acquire
  ///
  /// @return true if the orec was acquired, false otherwise
  bool acquire_consistent(orec_t *orec) {
    // Relaxed load is OK: we're going to CAS it
    auto val = orec->curr.load(std::memory_order_relaxed);
    if (val == my_lock)
      return true;
    if (unlikely(val > start_time)) { // NB: subsumes the LOCK_BIT check
      if (!is_locked(val))
        clock.on_abort(val);
      return false;
    }
    if (unlikely(!orec->curr.compare_exchange_strong(val, my_lock)))
      return false;

    orec->prev = val;
    locks.push_back(orec);
    return true;
  }

  /// A specialization of `acquire_consistent` that also reports if the orec was
  /// acquired before this call was made.
  ///
  /// @param orec   The orec to acquire
  /// @param locked A ref param to indicate if the location is locked
  ///
  /// @return true if the orec was acquired, false otherwise
  bool acquire_consistent(orec_t *orec, bool &locked) {
    // Relaxed load is OK: we're going to CAS it
    auto val = orec->curr.load(std::memory_order_relaxed);
    if (unlikely(val == my_lock)) {
      locked = true;
      return true;
    }
    if (unlikely(val > start_time)) {
      locked = is_locked(val);
      if (!locked)
        clock.on_abort(val);
      return false;
    }
    if (unlikely(!orec->curr.compare_exchange_strong(val, my_lock)))
      return false;
    orec->prev = val;
    locks.push_back(orec);
    return true;
  }

  /// Acquire an orec, but only if its value is still `val`
  ///
  /// @param orec The orec to acquire
  /// @param val  The expected value of the orec
  ///
  /// @return true if the orec was acquired, false otherwise
  bool acquire_continuation(orec_t *orec, uint64_t val) {
    // Relaxed load is OK: we're going to CAS it
    auto orec_val = orec->curr.load(std::memory_order_relaxed);
    if (unlikely(orec_val > val)) {
      if (orec_val == my_lock)
        return true;
      if (!is_locked(orec_val))
        clock.on_abort(orec_val);
      return false;
    }

    if (unlikely(!orec->curr.compare_exchange_strong(orec_val, my_lock)))
      return false;
    orec->prev = orec_val;
    locks.push_back(orec);
    return true;
  }

  /// Report if the current operation has acquired any orecs
  bool has_orecs() { return !locks.empty(); }

  /// Try to acquire an orec, without comparing its value to `this.start_time`.
  /// Return false on failure.
  ///
  /// @param orec The orec to acquire
  ///
  /// @return true if the orec was acquired, false otherwise
  bool acquire_aggressive(orec_t *orec) {
    // Relaxed load is OK: we're going to CAS it
    auto val = orec->curr.load(std::memory_order_relaxed);
    if (unlikely(val & LOCK_BIT)) // if it's locked, it had better be mine!
      return (val == my_lock);
    if (likely(orec->curr.compare_exchange_strong(val, my_lock))) {
      orec->prev = val;
      locks.push_back(orec);
      return true;
    }
    return false;
  }

  /// Stop using exoTM to write orecs, by advancing orec values to a new time
  ///
  /// NB: It is safe to call this after calling unwind.  That's nice for RAII
  ///     interfaces to exoTM.
  void wo_end() {
    // TODO:  This call will need to be in the unwind() code if we're going to
    //        support TL2-style optimized commit
    clock.on_end(global_clock);

    // If we've unwound, just exit
    if (unwound) {
      unwound = false;
      return;
    }

    // Read the clock.  We need an mfence before the rdtsc, so the write can't
    // be relaxed. (https://www.felixcloutier.com/x86/rdtscp)
    start_time = END_OF_TIME; // mfence
    last_wo_end_time = clock.increment_get(global_clock);

    // NB: There's an (essential) data dependence from the clock read to the
    //     lock release
    for (auto o : locks)
      o->curr.store(last_wo_end_time, std::memory_order_relaxed);
    locks.clear();
  }

  /// Undo writes to orecs.  Calling this will effectively transform wo_end into
  /// a no-op.
  ///
  /// @param how An enum indicating if orecs need to be bumped up (i.e., check
  ///            twice orecs with undo logging) or if they can be reset to their
  ///            old value (commit-time writeback).  There is no option for
  ///            "silent store" (undo + check-once orecs); if you need that, you
  ///            might as well commit.
  void unwind(UNWIND_TYPES how = ROLLBACK_ORECS) {
    unwound = true;
    start_time = END_OF_TIME; // mfence, so releases can be relaxed
    if (how == ROLLBACK_ORECS) {
      for (auto o : locks)
        o->curr.store(o->prev, std::memory_order_relaxed);
    } else {
      // NB: since we're using rdtsc, o->prev+1 won't exceed the clock
      // TODO: only need max under non-rdtsc clocks
      uintptr_t max = 0;
      for (auto o : locks) {
        uint64_t val = o->prev + 1;
        o->curr = val;
        max = (val > max) ? val : max;
      }
      clock.ensure_min(global_clock, max);
    }
    locks.clear();
  }

  /// Report the value returned by ro_begin() or wo_begin()
  uint64_t get_start_time() { return start_time; }

  /// Report the time of the last wo_end()
  uint64_t get_last_wo_end_time() { return last_wo_end_time; }

private:
  /// check if the orec is locked
  bool is_locked(uint64_t orec) { return orec & LOCK_BIT; }
};
