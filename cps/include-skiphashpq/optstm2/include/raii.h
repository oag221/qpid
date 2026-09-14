#pragma once

#include <exception>
#include <iostream>
#include <utility>

/// A lightweight RAII object for RO transactions.
/// - Statically identifies RO transactions, so we can get simpler RO get()
///   instrumentation
/// - Does some minimal enforcement of lexical scoping (within the limits of
///   OPTSTM2)
template <class DESCRIPTOR> class RoStm {
  // Fields need to be friends, so they can access `op`
  template <typename T, typename D> friend class field_base_t;
  template <class D> friend class RwStm;

  DESCRIPTOR * op; // The thread descriptor for this operation

public:
  /// Construct to start a read-only transaction
  ///
  /// @param me The thread descriptor
  RoStm(DESCRIPTOR *me) : op(me) { me->tx_ro_begin(); }

  /// Relinquish this transaction so that it can be adopted by an RwStm.  After
  /// this the RoStm is inert and its destructor does nothing.
  DESCRIPTOR *release() {
    DESCRIPTOR *r = op;
    op = nullptr;
    return r;
  }

  // MOVE constructor
  RoStm(RoStm&& other) noexcept : op(other.op) {
    other.op = nullptr;
  }

  // MOVE assignment operator
  RoStm& operator=(RoStm&& other) noexcept {
    if (this == &other) {
        return *this;
    }
    // Steal the resource
    this->op = other.op;
    // Nullify the source
    other.op = nullptr;

    return *this;
  }

  RoStm(const RoStm&) = delete;
  RoStm& operator=(const RoStm&) = delete;

  /// In OPTSTM2, we don't destruct to commit, so this just checks to make sure
  /// you committed / aborted.
  ~RoStm() {
    if (op && op->in_tx) {
      std::cout << "ERROR HERE (raii.h)\n";
      std::terminate();
    }
      
  }
  /// Provide the underlying OPTSTM2 descriptor
  DESCRIPTOR *OP() { return op; }
};

/// A lightweight RAII object for RW transactions.
/// - Statically identifies RW transactions, which need heavier get()
///   instrumentation
/// - Does some minimal enforcement of lexical scoping (within the limits of
///   OPTSTM2)
template <class DESCRIPTOR> class RwStm {
  // Fields need to be friends, so they can access `op`
  template <typename T, typename D> friend class field_base_t;

  DESCRIPTOR * op; // The thread descriptor for this operation

public:
  /// Construct to start a read/write transaction
  ///
  /// @param me The thread descriptor
  RwStm(DESCRIPTOR *me) : op(me) { op->tx_wo_begin(); }

  /// Adopt an in-flight read-only transaction, keeping its start time.  This
  /// saves a clock read and two fences relative to `end_ro(); RW rw(me);` for
  /// operations that read before they write.  Usage:
  ///     RO ro(me); ...reads...; RW rw(me, std::move(ro));
  template <class RO_T> RwStm(DESCRIPTOR *me, RO_T &&ro) : op(me) {
    ro.release();
    op->tx_upgrade_to_rw();
  }

  // MOVE constructor
  RwStm(RwStm&& other) noexcept : op(other.op) {
    other.op = nullptr;
  }

  // MOVE assignment operator
  RwStm& operator=(RwStm&& other) noexcept {
    if (this == &other) {
        return *this;
    }
    // Steal the resource
    this->op = other.op;
    // Nullify the source
    other.op = nullptr;

    return *this;
  }

  RwStm(const RwStm&) = delete;
  RwStm& operator=(const RwStm&) = delete;

  /// In OPTSTM2, we don't destruct to commit, so this just checks to make sure
  /// you committed / aborted.
  ~RwStm() {
    if (!(op && op->in_tx)) return;

    if (std::uncaught_exceptions() > 0) {
        // DO NOT TERMINATE.
        // Instead, silently clean up (e.g., abort/rollback the transaction).
        // This prevents hiding the original exception.
        op->in_tx = false; 
        // op->rollback(); // If you have a rollback function, call it here.
    } 
    else {
        // 3. No exception is flying, but the user forgot to commit/abort.
        // This is a logic error, so it is safe to terminate/assert.
        std::terminate(); 
    }
  }

  /// Whenever a node is speculatively allocated, use this to log it
  ///
  /// @param node The ownable_t to log
  ///
  /// @return `node`, to facilitate chaining
  template <class T> T *LOG_NEW(T *node) {
    if (!op->in_tx)
      std::terminate();
    this->op->mallocs.push_back(node);
    return node;
  }

  /// Schedule an object for reclamation if the transaction commits
  ///
  /// NB: It might seem odd that the reclamation is only for RW, and that it's
  ///     tied to the RW object instead of DESCRIPTOR itself.  It works for now.
  ///
  /// @param obj The object to reclaim
  void reclaim(typename DESCRIPTOR::ownable_t *obj) {
    this->op->frees.push_back(obj);
  }

  /// Provide the underlying OPTSTM2 descriptor
  DESCRIPTOR *OP() { return op; }
};
