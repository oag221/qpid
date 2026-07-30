#pragma once

#include <exception>

/// A lightweight RAII object for RO transactions.
/// - Statically identifies RO transactions, so we can get simpler RO get()
///   instrumentation
/// - Does some minimal enforcement of lexical scoping (within the limits of
///   OPTSTM2)
template <class DESCRIPTOR> class RoStm {
  // Fields need to be friends, so they can access `op`
  template <typename T, typename D> friend class field_base_t;

  DESCRIPTOR *const op; // The thread descriptor for this operation

public:
  /// Construct to start a read-only transaction
  ///
  /// @param me The thread descriptor
  RoStm(DESCRIPTOR *me) : op(me) {
    me->exo.ro_begin();
    me->in_tx = true;
  }
  /// In OPTSTM2, we don't destruct to commit, so this just checks to make sure
  /// you committed / aborted.
  ~RoStm() {
    if (op->in_tx)
      std::terminate();
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

  DESCRIPTOR *const op; // The thread descriptor for this operation

public:
  /// Construct to start a read/write transaction
  ///
  /// @param me The thread descriptor
  RwStm(DESCRIPTOR *me) : op(me) {
    op->exo.wo_begin();
    op->in_tx = true;
  }

  /// In OPTSTM2, we don't destruct to commit, so this just checks to make sure
  /// you committed / aborted.
  ~RwStm() {
    if (op->in_tx)
      std::terminate();
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
