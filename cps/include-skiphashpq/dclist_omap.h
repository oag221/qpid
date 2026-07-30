// 2024-11-20: This data structure is functional.  There are a few outstanding
// issues:
//
// -  The API may not be right, especially with regard to returning pointers to
//    collections
// -  Careful attention needs to be given to the destructors
// -  It would be worth re-thinking whether we want to hard-code assumptions
//    about emptiness.
// -  Deletion of nodes that become empty is probably inefficient due to two
//    searches in the transaction.

#pragma once

#include <iostream>
#include <optional>

/// An ordered map of collection objects, implemented as a doubly-linked list.
/// Note that since the map holds *collections*, the API is slightly different
/// than a classic omap of K/V pairs.  In particular, `get()` returns a
/// pointer to a collection, and instead of `insert()`, we have a
/// `make_collection()` method for upserting an empty collection into the list.
///
/// @param K      The type of the keys stored in this map
/// @param C      The type of the collection stored in this map
/// @param OPTSTM A thread descriptor type, for safe memory reclamation
template <typename K, typename C, class OPTSTM> class dclist_omap {//! C = QD_T = chunk_queue_diffractor
  using ownable_t = typename OPTSTM::ownable_t;
  // NB: `FIELD` is unused, but it's nice to have it anyway
  template <typename T> using FIELD = typename OPTSTM::template xField<T>;
  using ROTX = typename OPTSTM::RO;
  using RWTX = typename OPTSTM::RW;

  /// A list node.  It has prev and next pointers, but no key or value.  It's
  /// useful for sentinels, so that K and V don't have to be default
  /// constructable.
  struct node_t : ownable_t {
    FIELD<node_t *> prev; // Pointer to predecessor
    FIELD<node_t *> next; // Pointer to successor

    /// Construct a node
    node_t() : prev(nullptr), next(nullptr) {}

    /// Destructor is a no-op, but it needs to be virtual because of inheritance
    virtual ~node_t() {}
  };

  /// A list node that also has a key and collection.  Note that keys are const,
  /// and collections are inlined, thus they aren't FIELDs.
  struct data_t : public node_t {
    const K key;  // The key of this pair
    C collection; // The value of this pair

    /// Construct a data_t
    ///
    /// @param rw   A writing transactional context, for making the collection
    /// @param _key The key that is stored in this node
    /// @param size The default size to pass to the collection's constructor
    data_t(RWTX &rw, const K &_key, uint32_t size, uint32_t n_queues)
        : node_t(), key(_key), collection(rw, size, n_queues) {}

    /// Destructor is a no-op, but it needs to be virtual because of inheritance
    virtual ~data_t() {}
  };

  node_t *const head;   // The list head pointer
  node_t *const tail;   // The list tail pointer
  const uint32_t csize; // The size to pass to the collection constructor
  const uint32_t num_queues; // Number of queues

public:
  /// Default construct a list by constructing and connecting two sentinel nodes
  ///
  /// @param me  The operation that is constructing the list
  /// @param cfg A configuration object
  template <typename config_t>
  dclist_omap(OPTSTM *me, config_t *cfg)
      : head(new node_t()), tail(new node_t()), csize(cfg->chunksize), num_queues(cfg->num_queues)
  {
    RWTX rw(me);
    head->next.set_cap(rw, head, tail);
    tail->prev.set_cap(rw, tail, head);
    if (!rw.OP()->try_end_rw())
      std::terminate();
  }

  ~dclist_omap() {
    delete head;
    delete tail;
  }

private:
  /// get_leq is an inclusive predecessor query that returns the largest node
  /// whose key is <= the provided key.  It can return the head sentinel, but
  /// not the tail sentinel.
  ///
  /// @tparam TX The type of the active transaction (RO or RW)
  ///
  /// @param tx  The active transaction
  /// @param key The key for which we are doing a predecessor query.
  ///
  /// @return The node that was found
  template <class TX> std::optional<node_t *> get_leq(TX &tx, const K key) {
    // Start at the head; read the next now, to avoid reading it in multiple
    // iterations of the loop
    node_t *curr = head;
    auto next_o = curr->next.get(tx, curr);
    if (!next_o)
      return {};
    auto next = next_o.value();

    // Starting at `next`, search for key.
    while (true) {
      // Case 1: `next` is tail --> stop the search at curr
      if (next == tail) {
        // don't need `prev` to be set in this case
        return curr;
      }
        
      // read next's `key`
      auto nkey = static_cast<data_t *>(next)->key;

      // Case 2: `next` is a data node: stop if next->key >= key
      if (nkey > key) {
        return curr;
      }
      if (nkey == key) {
        return next;
      }

      // read next's `next`
      auto next_next_o = next->next.get(tx, next);
      if (!next_next_o)
        return {};
      auto next_next = next_next_o.value();

      // Case 3: keep traversing to `next`
      curr = next;
      next = next_next;
    }
  }

public:
  /// Search the data structure for a node with key `key`.  If not found, return
  /// false.  If found, return a reference with the associated collection.
  ///
  /// @tparam TX The type of the active transaction (RO or RW)
  ///
  /// @param tx  The active transactional context
  /// @param key The key to search
  /// @param val A ref parameter for returning key's value, if found
  ///
  /// @return A reference to the collection if found, nullptr if not, and NONE
  ///         on abort
  template <class TX> std::optional<C *> get_extract(TX &tx, const K &key) {
    // get_leq will use a read-only transaction to find the largest node with
    // a key <= `key`.
    auto n_o = get_leq(tx, key);
    if (!n_o)
      return {}; // ABORT!
    auto n = n_o.value();

    // Since we have EBR, we can read n.key without validating and fast-fail
    // on key-not-found
    if (n == head || static_cast<data_t *>(n)->key != key)
      return {nullptr};

    // NB:  Given EBR, the `collection` field is pinned, so we can return a
    //      pointer to it
    data_t *dn = static_cast<data_t *>(n);
    return {&dn->collection};
  }

  template <class TX> std::optional<C *> get_ins(TX &tx, const K &key) {
    // get_leq will use a read-only transaction to find the largest node with
    // a key <= `key`.
    auto n_o = get_leq(tx, key);
    if (!n_o)
      return {}; // ABORT!
    auto n = n_o.value();

    // Since we have EBR, we can read n.key without validating and fast-fail
    // on key-not-found
    if (n == head || static_cast<data_t *>(n)->key != key)
      return {nullptr};

    // NB:  Given EBR, the `collection` field is pinned, so we can return a
    //      pointer to it
    data_t *dn = static_cast<data_t *>(n);
    return {&dn->collection};
  }

  /// "Upsert" an empty collection for `key` and return a reference to it
  ///
  /// @param rw  A writing transactional context
  /// @param key The key for the mapping to upsert
  ///
  /// @return A reference to the collection for `key`, or NONE on abort
  std::optional<C *> make_collection(RWTX &rw, const K &key) {
    auto n_o = get_leq(rw, key);
    if (!n_o)
      return {}; // ABORT!
    // Since this is an upsert, if we find it, we return it
    auto n = n_o.value();
    if (n != head && static_cast<data_t *>(n)->key == key)
      return {&static_cast<data_t *>(n)->collection};

    // Start the insertion by getting the successor
    auto next_o = n->next.get(rw, n);
    if (!next_o)
      return {}; // ABORT!
    auto next = next_o.value();

    // stitch in a new node
    data_t *new_dn = rw.LOG_NEW(new data_t(rw, key, csize, num_queues));
    new_dn->next.set_cap(rw, new_dn, next);
    new_dn->prev.set_cap(rw, new_dn, n);
    if (!n->next.set(rw, n, new_dn) || !next->prev.set(rw, next, new_dn))
      return {}; // ABORT
    // Return a pointer to the new node's collection
    return {&new_dn->collection};
  }

  /// Remove the node associated with `key`
  ///
  /// @param rw  A writing transactional context
  /// @param key The key for the mapping to eliminate
  ///
  /// @return True if the key was found and removed, false otherwise, NONE on
  ///         abort
  std::optional<bool> remove(RWTX &rw, const K &key) {
    auto n_o = get_leq(rw, key);
    if (!n_o)
      return {}; // ABORT!
    auto n = n_o.value();
    if (n == head || static_cast<data_t *>(n)->key != key)
      return {false}; // NOT FOUND!

    // unstitch it
    auto pred_o = n->prev.get(rw, n);
    if (!pred_o)
      return {}; // ABORT!
    auto succ_o = n->next.get(rw, n);
    if (!succ_o)
      return {}; // ABORT!
    auto pred = pred_o.value(), succ = succ_o.value();
    if (!pred->next.set(rw, pred, succ) || !succ->prev.set(rw, succ, pred))
      return {}; // ABORT!
    rw.reclaim(n);
    return {true};
  }

  std::pair<long,long> dump(ROTX &ro, bool print=false) {
    // Start at the head; read the next now, to avoid reading it in multiple
    // iterations of the loop
    data_t *curr = (data_t *)head->next.get(ro, head).value();
    long num_elems = 0;
    long key_sum = 0;

    while (curr != tail) {
      if (print) std::cout << "[" << curr->key << "] :: ";
      std::pair<long,long> ret = curr->collection.dump(ro, print);
      num_elems += ret.first;
      key_sum += ret.second;
      if (print) std::cout << "curr->key: " << curr->key << ", KEYSUM: " << key_sum << "\n";
      curr = (data_t *)curr->next.get(ro, curr).value();
    }
    // std::cout << "HERE-returning!\n";
    return std::make_pair(num_elems, key_sum);
  }

  std::vector<std::pair<K,long>> dump_ht() {
    std::vector<std::pair<K,long>> ret;
    data_t *curr = (data_t *)head->next.get_unsafe();
    while (curr != tail) {
      long ret_coll = curr->collection.dump_ht();
      ret.push_back(std::make_pair(curr->key, ret_coll));
      curr = (data_t *)curr->next.get_unsafe();
    }
    return ret;
  }
};
