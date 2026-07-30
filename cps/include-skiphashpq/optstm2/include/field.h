// last review: 01-26-2023

#pragma once

#include <optional>

/// field_base_t has the code that is shared among all of our OptSTM2 policies'
/// field implementations
///
/// @tparam T   The type that is tored in this field_base_t
/// @tparam TX  The active transaction's RAII object
template <typename T, typename TX> class field_base_t {
protected:
  T _val; // The value.  It's going to be cast to atomic by the undolog.

  /// Construct a field_base_t
  ///
  /// @param val The initial value
  explicit field_base_t(T val) : _val(val) {}

  /// Default-construct a field_base_t
  explicit field_base_t() : _val() {}

public:
  /// Write to shared memory (captured / not-actually-shared memory)
  ///
  /// @param tx  The transaction performing this read
  /// @param o   The ownable for this location (locates the orec)
  /// @param val The new value
  void set_cap(typename TX::RW &tx, typename TX::OWNABLE *o, T val) {
    _val = val;
  }

  /// Write to shared memory (captured / not-actually-shared memory)
  ///
  /// @param tx  The transaction performing this read
  /// @param o   The ownable for this location (locates the orec)
  /// @param val The new value
  void set_unsafe(T val) {
    _val = val;
  }

  /// Read unsafely from memory
  ///
  /// @return The current value
  [[nodiscard]] T get_unsafe() {
    return _val;
  }
};

/// eager_field_t extends field_base_t with code that is shared among the eager
/// OptSTM2 policies' field implementations
///
/// @tparam T          The type that is stored in this eager_field
/// @tparam TX The type of OptSTM2 policy using this field
template <typename T, typename TX>
class eager_field_t : public field_base_t<T, TX> {
protected:
  /// Construct an eager_c1_field
  ///
  /// @param val The initial value
  explicit eager_field_t(T val) : field_base_t<T, TX>(val) {}

  /// Default-construct an eager_c1_field
  explicit eager_field_t() : field_base_t<T, TX>() {}

public:
  /// Read from shared memory (middle read in a sequence of reads of `o` by
  /// `tx`, without any control flow / computed addresses)
  ///
  /// @param tx  The transaction performing this read
  /// @param o   The ownable for this location (locates the orec)
  ///
  /// @return The current value
  template <class TM> T get_in_seq(TM &tx, typename TX::OWNABLE *o) {
    return tx.OP()->undolog.safe_read(&this->_val);
  }

  /// Read from shared memory (`o` is owned by `tx`)
  ///
  /// @param tx  The transaction performing this read
  /// @param o   The ownable for this location (locates the orec)
  ///
  /// @return The current value
  [[nodiscard]] std::optional<T> get_mine(typename TX::RW &tx,
                                          typename TX::OWNABLE *o) {
    if (!tx.OP()->in_tx)
      std::terminate();
    return tx.OP()->undolog.safe_read(&this->_val);
  }

  /// Write to shared memory (general-purpose version)
  ///
  /// @param tx  The transaction performing this read
  /// @param o   The ownable for this location (locates the orec)
  /// @param val The new value
  [[nodiscard]] bool set(typename TX::RW &tx, typename TX::OWNABLE *o, T val) {
    if (!tx.OP()->in_tx)
      std::terminate();
    while (true) {
      // If I have it or can get it, that's the easy case
      bool locked = false;
      if (tx.OP()->exo.acquire_consistent(o->orec(), locked)) {
        typename TX::UNDO_T u;
        u.initFromAddr(&this->_val);
        tx.OP()->undolog.push_back(u);
        tx.OP()->undolog.safe_write(&this->_val, val);
        return true;
      }

      // abort if locked
      if (locked) {
        tx.OP()->unwind();
        return false;
      }

      // Extend the validity range, then try again
      auto old_start = tx.OP()->exo.get_start_time();
      tx.OP()->exo.wo_begin();
      if (!tx.OP()->is_valid(old_start))
        return false;
    }
  }

  /// Write to shared memory (`o` is owned by `tx`)
  ///
  /// @param tx  The transaction performing this read
  /// @param o   The ownable for this location (locates the orec)
  /// @param val The new value
  void set_mine(typename TX::RW &tx, typename TX::OWNABLE *o, T val) {
    if (!tx.OP()->in_tx)
      std::terminate();
    typename TX::UNDO_T u;
    u.initFromAddr(&this->_val);
    tx.OP()->undolog.push_back(u);
    tx.OP()->undolog.safe_write(&this->_val, val);
  }
};

/// eager_noext_field_t extends field_base_t with code that is shared among the
/// field implementations of eager OPTSTM2 policies that do not use timestamp
/// extension
///
/// @tparam T          The type that is stored in this eager_noext_field
/// @tparam TX The type of OptSTM2 policy using this field
template <typename T, typename TX>
class eager_noext_field_t : public field_base_t<T, TX> {
protected:
  /// Construct an eager_noext_field_t
  ///
  /// @param val The initial value
  explicit eager_noext_field_t(T val) : field_base_t<T, TX>(val) {}

  /// Default-construct an eager_noext_field_t
  explicit eager_noext_field_t() : field_base_t<T, TX>() {}

public:
  /// Read from shared memory (middle read in a sequence of reads of `o` by
  /// `tx`, without any control flow / computed addresses)
  ///
  /// @param tx  The transaction performing this read
  /// @param o   The ownable for this location (locates the orec)
  ///
  /// @return The current value
  template <class TM> T get_in_seq(TM &tx, typename TX::OWNABLE *o) {
    if (!tx.OP()->in_tx)
      std::terminate();
    return tx.OP()->undolog.safe_read(&this->_val);
  }

  /// Read from shared memory (`o` is owned by `tx`)
  ///
  /// @param tx  The transaction performing this read
  /// @param o   The ownable for this location (locates the orec)
  ///
  /// @return The current value
  [[nodiscard]] std::optional<T> get_mine(typename TX::RW &tx,
                                          typename TX::OWNABLE *o) {
    if (!tx.OP()->in_tx)
      std::terminate();
    return tx.OP()->undolog.safe_read(&this->_val);
  }

  /// Write to shared memory (general-purpose version)
  ///
  /// @param tx  The transaction performing this read
  /// @param o   The ownable for this location (locates the orec)
  /// @param val The new value
  [[nodiscard]] bool set(typename TX::RW &tx, typename TX::OWNABLE *o, T val) {
    if (!tx.OP()->in_tx)
      std::terminate();

    // If I have it or can get it, that's the easy case
    bool locked = false;
    if (tx.OP()->exo.acquire_consistent(o->orec(), locked)) {
      typename TX::UNDO_T u;
      u.initFromAddr(&this->_val);
      tx.OP()->undolog.push_back(u);
      tx.OP()->undolog.safe_write(&this->_val, val);
      return true;
    }
    // It's locked or too new.  Abort in both cases
    tx.OP()->unwind();
    return false;
  }

  /// Write to shared memory (`o` is owned by `tx`)
  ///
  /// @param tx  The transaction performing this read
  /// @param o   The ownable for this location (locates the orec)
  /// @param val The new value
  void set_mine(typename TX::RW &tx, typename TX::OWNABLE *o, T val) {
    if (!tx.OP()->in_tx)
      std::terminate();
    typename TX::UNDO_T u;
    u.initFromAddr(&this->_val);
    tx.OP()->undolog.push_back(u);
    tx.OP()->undolog.safe_write(&this->_val, val);
  }
};

/// wb_field_t is a wrapper around simple types so that they can only be
/// accessed via OptSTM2.
///
/// @tparam T          The type that is stored in this eager_c1_field
/// @tparam TX The type of OptSTM2 policy using this field
template <typename T, typename TX>
class wb_field_t : public field_base_t<T, TX> {
public:
  /// Construct a wb_field_t
  ///
  /// @param val The initial value
  explicit wb_field_t(T val) : field_base_t<T, TX>(val) {}

  /// Default-construct a wb_field_t
  explicit wb_field_t() : field_base_t<T, TX>() {}

  /// Read from shared memory (middle read in a sequence of reads of `o` by
  /// `tx`, without any control flow / computed addresses)
  ///
  /// @param tx  The transaction performing this read
  /// @param o   The ownable for this location (locates the orec)
  ///
  /// @return The current value
  template <class TM> T get_in_seq(TM &tx, typename TX::OWNABLE *o) {
    if (!tx.OP()->in_tx)
      std::terminate();
    T ret;
    if (tx.OP()->redolog.get(&this->_val, ret))
      return ret;
    return tx.OP()->redolog.safe_read(&this->_val);
  }

  /// Read from shared memory (`o` is owned by `tx`)
  ///
  /// @param tx  The transaction performing this read
  /// @param o   The ownable for this location (locates the orec)
  ///
  /// @return The current value
  [[nodiscard]] std::optional<T> get_mine(typename TX::RW &tx,
                                          typename TX::OWNABLE *o) {
    if (!tx.OP()->in_tx)
      std::terminate();
    T ret;
    if (tx.OP()->redolog.get(&this->_val, ret))
      return ret;
    return tx.OP()->redolog.safe_read(&this->_val);
  }

  /// Write to shared memory (general-purpose version)
  ///
  /// @param tx  The transaction performing this read
  /// @param o   The ownable for this location (locates the orec)
  /// @param val The new value
  [[nodiscard]] bool set(typename TX::RW &tx, typename TX::OWNABLE *o, T val) {
    if (!tx.OP()->in_tx)
      std::terminate();
    // Put it in the redo log right away
    tx.OP()->redolog.insert(&this->_val, val);

    // Now either consistently get the lock, or else abort
    while (true) {
      // If I have it or can get it, that's the easy case
      bool locked = false;
      if (tx.OP()->exo.acquire_consistent(o->orec(), locked))
        return true;

      // abort if locked
      if (locked) {
        tx.OP()->unwind();
        return false;
      }

      // Extend the validity range, then try again
      auto old_start = tx.OP()->exo.get_start_time();
      tx.OP()->exo.wo_begin();
      if (!tx.OP()->is_valid(old_start))
        return false;
    }
  }

  /// Write to shared memory (`o` is owned by `tx`)
  ///
  /// @param tx  The transaction performing this read
  /// @param o   The ownable for this location (locates the orec)
  /// @param val The new value
  void set_mine(typename TX::RW &tx, typename TX::OWNABLE *o, T val) {
    if (!tx.OP()->in_tx)
      std::terminate();
    tx.OP()->redolog.insert(&this->_val, val);
  }
};
