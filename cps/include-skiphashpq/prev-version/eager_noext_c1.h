// Last Review: Needs Review

#pragma once

#include "../exotm/exotm.h"
#include "../include/hash.h"
#include "../include/orec_policies.h"
#include "../include/rdtsc_rand.h"
#include "../include/timestamp_smr.h"
#include "../include/undolog.h"
#include "include/field.h"
#include "include/raii.h"

/// eager_noext_c1_t is an OptSTM2 policy with the following features:
/// - Uses ExoTM for orecs and a clock
/// - Check-once orecs
/// - Encounter-time locking with undo
/// - No quiescence, but safe memory reclamation
/// - Can be configured with per-object or per-stripe orecs
/// - No timestamp extension (and thus really simple read instrumentation)
///
/// @tparam OP    The orec policy to use (PO or PS)
/// @tparam CLOCK The clock mechanism to use
template <template <typename, typename> typename OP, class CLOCK>
class eager_noext_c1_t {
  using exotm_t = exotm_t<CLOCK>;
  using orec_t = typename exotm_t::orec_t;                       // Orec type
  using OrecPolicy = OP<timestamp_smr_t::reclaimable_t, orec_t>; // Orec Policy

  /// A packet storing all globals for undo OptSTM2 policies
  struct global_t {
    timestamp_smr_t::global_t smr;    // Globals for safe memory reclamation
    typename OrecPolicy::global_t op; // Globals for the orec policy
  };

  /// lightweight singleton-like access to the globals
  static global_t _globals;

public:
  using RO = RoStm<eager_noext_c1_t>; // RAII RO STM manager
  using RW = RwStm<eager_noext_c1_t>; // RAII RW STM manager

  /// ownable_t from OP, but with a zero-argument constructor.
  struct ownable_t : public OrecPolicy::ownable_t {
    /// Construct an ownable_t
    ownable_t() : OrecPolicy::ownable_t(_globals.op) {}
  };

private:
  exotm_t exo;                     // The thread's exoTM context
  timestamp_smr_t smr;             // The safe memory reclamation context
  rdtsc_rand_t rng;                // A random number generator
  minivector<orec_t *> readset;    // Orecs to validate
  undolog_t undolog;               // An undo log, for undoing writes on abort
  minivector<ownable_t *> mallocs; // pending allocations
  minivector<ownable_t *> frees;   // pending reclaims
  bool in_tx = false;              // Track if we're in a TX, for debugging

  /// Ensure that all orecs that we've read have timestamps older than the start
  /// time, unless we locked those orecs. If we locked the orec, we did so when
  /// the time was smaller than our start time, so we're sure to be OK.
  [[nodiscard]] bool is_valid() {
    if (!in_tx)
      std::terminate();
    // NB: on relaxed architectures, we may have unnecessary fences here
    for (auto o : readset)
      if (exo.check_orec(o) == exotm_t::END_OF_TIME) {
        unwind();
        return false;
      }
    return true;
  }

  /// Specialized version of validation for timestamp extension.  Compare
  /// against old_start, not exo.start_time.
  [[nodiscard]] bool is_valid(uint64_t old_start) {
    if (!in_tx)
      std::terminate();
    // NB: on relaxed architectures, we may have unnecessary fences here
    for (auto o : readset) {
      bool mine = false;
      bool ok = exo.check_continuation(o, old_start, mine);
      if (!ok && !mine) {
        unwind();
        return false;
      }
    }
    return true;
  }

  /// Unwind the transaction
  void unwind() {
    if (!in_tx)
      std::terminate();
    in_tx = false;
    // A wo operation might not have orecs yet, but have some scheduled
    // frees/mallocs that need to be undone.  We can undo mallocs right away
    frees.clear();
    for (auto p : mallocs)
      smr.reclaim(p);
    mallocs.clear();

    // It's safe to assume that if there is an abort, there's at least one read
    readset.clear();

    // In C1, we need to commit as silent store to release locks.  Even if there
    // are no locks, this is going to bump the counter, so dodge it if there are
    // no writes yet
    if (!exo.has_orecs()) { // does not have any locks
      exo.ro_end();
      if (undolog.size() > 0)
        std::terminate();
    } else {
      undolog.undo_writes();
      exo.wo_end();
      undolog.clear();
    }
  }

  /// Try to commit a writing transaction
  bool try_commit() {
    if (!in_tx)
      std::terminate();
    // read-only fast-path
    if (!exo.has_orecs()) {
      exo.ro_end();
      mallocs.clear();
      for (auto a : frees)
        smr.reclaim(a); // Need SMR here!
      frees.clear();
      readset.clear();
      return true;
    }

    // Locks are already acquired, so just validate.  If false, it unwinds.
    if (!is_valid())
      return false;

    // We're committed, so release locks and clean up
    exo.wo_end();
    mallocs.clear();
    for (auto a : frees)
      smr.reclaim(a); // Need SMR here!
    frees.clear();
    readset.clear();
    undolog.clear();
    return true;
  }

public:
  /// Construct an eager_noext_c1_t
  eager_noext_c1_t() : exo(), smr(_globals.smr) {}

  /// Start an operation (notify SMR)
  void op_begin() { smr.enter(); }

  /// End an operation (notify SMR)
  void op_end() { smr.exit(_globals.smr); }

  /// A good hash function.  Works nicely to "finalize" after std::hash().
  ///
  /// @param val The value to hash
  ///
  /// @return A 64-bit hash value
  uint64_t hash(size_t val) { return mix13_hash(val); }

  /// Produce a random number from a thread-local generator
  int rand() { return rng.rand(); }

  /// The type for fields that are shared and protected by OptSTM2
  ///
  /// @tparam T The type that is stored in this xField
  template <typename T>
  struct xField : public eager_noext_field_t<T, eager_noext_c1_t> {
    /// Construct an xField
    ///
    /// @param val The initial value
    explicit xField(T val) : eager_noext_field_t<T, eager_noext_c1_t>(val) {}

    /// Default-construct an xField
    explicit xField() : eager_noext_field_t<T, eager_noext_c1_t>() {}

    /// Read from shared memory (general-purpose version)
    ///
    /// @param tx  The transaction performing this read
    /// @param o   The ownable for this location (locates the orec)
    ///
    /// @return The current value
    [[nodiscard]] std::optional<T> get(RW &tx, ownable_t *o) {
      if (!tx.OP()->in_tx)
        std::terminate();
      // read the location, then orec
      T from_mem = tx.OP()->undolog.safe_read(&this->_val);
      bool locked = false;
      auto post = tx.OP()->exo.check_orec(o->orec(), locked);
      // If validation passes, then we can log it and return
      if (post != EOT) {
        if (!locked)
          // [mfs] This logging is only needed for WO, not for RO.
          tx.OP()->readset.push_back(o->orec());
        return from_mem;
      }
      // It's locked or too new.  Abort in eithe rcase
      tx.OP()->unwind();
      return {};
    }

    /// Read from shared memory (RO transaction)
    ///
    /// @param tx  The transaction performing this read
    /// @param o   The ownable for this location (locates the orec)
    ///
    /// @return The current value
    [[nodiscard]] std::optional<T> get(RO &tx, ownable_t *o) {
      if (!tx.OP()->in_tx)
        std::terminate();
      // read the location, then orec
      T from_mem = tx.OP()->undolog.safe_read(&this->_val);
      bool locked = false;
      auto post = tx.OP()->exo.check_orec(o->orec(), locked);
      // If validation passes, then we can log it and return
      if (post != EOT) {
        // NB: No logging, because no timestam extension
        return from_mem;
      }
      // It's locked or too new.  Abort in eithe rcase
      tx.OP()->unwind();
      return {};
    }

    /// Read from shared memory (guaranteed not the first read to `o` by `tx`)
    ///
    /// @param tx  The transaction performing this read
    /// @param o   The ownable for this location (locates the orec)
    ///
    /// @return The current value
    template <class TM>
    [[nodiscard]] std::optional<T> re_get(TM &tx, ownable_t *o) {
      if (!tx.OP()->in_tx)
        std::terminate();
      // read the location, then orec
      T from_mem = tx.OP()->undolog.safe_read(&this->_val);
      // If validation fails, abort, else return the value without logging
      if (tx.OP()->exo.check_orec(o->orec()) == EOT) {
        tx.OP()->unwind();
        return {};
      }
      return from_mem;
    }
  };

  /// Try to commit a write-capable transaction.  Returns false if retry needed
  [[nodiscard]] bool try_end_rw() {
    if (!in_tx)
      std::terminate();
    auto res = this->try_commit();
    this->in_tx = false;
    return res;
  }

  /// Commit a read-only transaction.  Always succeeds
  void end_ro() {
    if (!in_tx)
      std::terminate();
    this->in_tx = false;
    this->exo.ro_end();
    this->readset.clear();
  }

private:
  // Types needed by the (friend) field template, but not worth making public
  static const auto EOT = exotm_t::END_OF_TIME;
  using OWNABLE = ownable_t;
  using UNDO_T = undolog_t::undo_t;

  // These friends ensure that the rest of the API can access the parts of this
  // that they need
  template <typename T, typename DESCRIPTOR> friend class field_base_t;
  template <typename T, typename DESCRIPTOR> friend class eager_noext_field_t;
  template <typename DESCRIPTOR> friend class RoStm;
  template <typename DESCRIPTOR> friend class RwStm;
};

/// OPTSTM2_GLOBALS_INITIALIZER should be called once, in the main C++ file of a
/// program.  It defines the globals used by undo OptSTM2 policies, so that we
/// can be sure that any globals declared in this file are defined in a .o file.
/// Failure to use this correctly will lead to link errors.
#define OPTSTM2_GLOBALS_INITIALIZER                                            \
  template <class CLOCK>                                                       \
  typename exotm_t<CLOCK>::CLOCK_T::global_t exotm_t<CLOCK>::global_clock;     \
  template <template <typename, typename> typename T, class C>                 \
  typename eager_noext_c1_t<T, C>::global_t eager_noext_c1_t<T, C>::_globals
