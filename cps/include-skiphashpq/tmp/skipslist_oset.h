// 2024-11-15: This data structure is functional.  There are a few outstanding
// issues:
//
// -  The use of level_t is unnecessary, though 0-overhead
// -  The API for insert_guaranteed allows for freeing and reallocating the new
//    node with different heights on each attempt.
// -  The insertion routine could be faster if we inserted as we traversed
// -  The API admits the possibility of re-reading things without a re_get()
//    optimization.
// -  The insert_guaranteed function could be flattened (and then, perhaps,
//    optimized with re_get() and re_set()?)
// -  The insert_guaranteed function is not leveraging set_cap() on the new node
// -  Will any of these methods be called from a ROTX, or can we just use RWTX
//    everywhere?

#pragma once

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <optional>
#include <type_traits>

/// An ordered set, implemented as a singly-linked skip list, optimized for a
/// priority queue.  This map supports empty(), front(), insert_guaranteed(),
/// and remove_front() operations.
///
/// @param K          The type of the keys stored in this map
/// @param OPTSTM     A thread descriptor type, for safe memory reclamation
template <typename K, class OPTSTM> class skipslist_oset {
  using ownable_t = typename OPTSTM::ownable_t;
  template <typename T> using FIELD = typename OPTSTM::template xField<T>;
  using ROTX = typename OPTSTM::RO;
  using RWTX = typename OPTSTM::RW;

  /// sl_node_t is a node in the skip list.  It has a key, an orec, and a
  /// "tower" of successor pointers
  struct sl_node_t : ownable_t {
    /// The successor at a level of the tower.
    ///
    /// [mfs] Wrapping in a struct is vestigial... clean up eventually
    struct level_t {
      FIELD<sl_node_t *> next; // Successor at this level
    };

    const K key;          // The key stored in this node
    const uint8_t height; // The allocated height of the tower
    level_t tower[0];     // Tower of pointers to pred/succ

  private:
    /// Construct a skip list node.  This is private to force the use of our
    /// make_* methods, which handle allocating enough space for the tower.
    ///
    /// @param _key The key that is stored in this node
    sl_node_t(K _key, uint8_t _height) : key(_key), height(_height) {}

  public:
    /// Construct a sentinel (head or tail) node.  Not transactional, because we
    /// assume you don't *make* skip lists inside of a transaction.
    ///
    /// @param iHeight  The max number of index layers this node will have
    static sl_node_t *make_sentinel(uint8_t iHeight) {
      int node_size = sizeof(sl_node_t) + (iHeight + 1) * sizeof(level_t);
      void *region = malloc(node_size);
      return new (region) sl_node_t(-1, iHeight);
    }

    /// Construct a data node from within a transactional context
    ///
    /// @param rw       A writing transactional context
    /// @param iHeight  The max number of index layers this node will have
    /// @param key      The key to store in this node
    static sl_node_t *make_data(RWTX &rw, uint64_t iHeight, K key) {
      int node_size = sizeof(sl_node_t) + (iHeight + 1) * sizeof(level_t);
      void *region = ::operator new(node_size);
      return rw.LOG_NEW(new (region) sl_node_t(key, iHeight));
    }
  };

  enum order_t {
    INCREASING, // PQ ordered in increasing order (smallest value = highest priority)
    DECREASING // PQ ordered in decreasing order (largest value = highest priority)
  };

  /// The maximum number of levels the user is allowed to set for the skip list.
  /// 32 levels allows 2^32 ~= 4 billion elements, which is probably enough.
  static constexpr uint8_t MAX_LAYERS = 32;

  const int NUM_INDEX_LAYERS; // # of index layers.  Doesn't count data layer
  order_t ORDER;
  sl_node_t *const head;      // The head sentinel
  sl_node_t *const tail;      // The tail sentinel

public:
  /// Default construct a skip list by stitching a head sentinel to a tail
  /// sentinel at each level.  This must be called from *within* a RW
  /// transactional context, because the setters (even for captured memory)
  /// require a RW context.
  ///
  /// @param rw   The currently active RW transaction
  /// @param cfg  A configuration object that has a `max_levels` field
  template <typename config_t>
  skipslist_oset(OPTSTM *me, config_t *cfg)
      : NUM_INDEX_LAYERS(cfg->max_levels),
        ORDER(order_t::INCREASING),
        head(sl_node_t::make_sentinel(NUM_INDEX_LAYERS)),
        tail(sl_node_t::make_sentinel(NUM_INDEX_LAYERS)) {
    if (NUM_INDEX_LAYERS > MAX_LAYERS)
      std::terminate();

    // cfg->order = 0 ==> order_t::INCREASING
    // cfg->order = 1 ==> order_t::DECREASING
    if (cfg->order) {
      ORDER = order_t::DECREASING;
    }

    RWTX rw(me);
    for (auto i = 0; i <= NUM_INDEX_LAYERS; i++)
      head->tower[i].next.set_cap(rw, head, tail);
    if (!me->try_end_rw())
      std::terminate();
  }

  /// Insert the provided `key`, with the assertion that the key is not already
  /// present.
  ///
  /// [mfs] This currently re-computes the height on each attempt!
  ///
  /// [mfs] This currently does *not* insert as it goes along.  Should we change
  ///       that?
  ///
  /// @param rw  A read/write STM context
  /// @param key The key to insert
  ///
  /// @return True if the value was inserted, false on abort
  bool insert_guaranteed(RWTX &rw, const K &key) {
    // The target index height of new_dn
    int const target_height = randomLevel(rw.OP());

    // Preallocate an array of pointers to record the predecessor at each level.
    //
    // [mfs]  Not sure if this is legal, since NUM_INDEX_LAYERS is not
    //        constexpr. But if we insert as we go along, it won't matter
    //        anyway.
    sl_node_t *preds[MAX_LAYERS];

    // Get the insertion point
    if (!skip_insert(rw, key, preds, target_height))
      return false; // ABORT!

    // Make a new node and stitch it in
    return sl_stitch(rw, preds, sl_node_t::make_data(rw, target_height, key));
  }

  /// Remove the first element of the skip list.
  ///
  /// NB: It is assumed that the caller has already ensured the skip list is not
  ///     empty.
  ///
  /// [mfs] The "NB" implies some re-reading?
  ///
  /// @param rw  A read/write transactional context
  ///
  /// @return True if the remove succeeded, false if it aborted
  bool remove_front(RWTX &rw, K expected_key) {
    // Read head's successor in the data layer, abort if the read fails
    auto target_o = head->tower[0].next.get(rw, head);
    if (!target_o)
      return false;
    // The head's successor is the target to unstitch
    auto target = target_o.value();
    if (expected_key != target->key) {
      std::cout << "front key = " << target->key << ", expected key = " << expected_key << "\n";
    }
    
    // Starting at target's height, unstitch it from the head node
    for (int level = target->height; level >= 0; --level) {
      // Get the target's successor or abort
      // [mfs] Optimize with re-get?
      auto succ_o = target->tower[level].next.get(rw, target);
      if (!succ_o)
        return false;
      // Update the head sentinel's next at this level or abort
      if (!head->tower[level].next.set(rw, head, succ_o.value()))
        return false;
    }
    // Reclaim it :)
    rw.reclaim(target);
    return true;
  }

  /// Return the value of the first element in the skip list.
  ///
  /// NB: It is assumed that the caller has already ensured the skip list is not
  ///     empty.
  ///
  /// [mfs] The "NB" implies some re-reading?
  ///
  /// @tparam TX the transaction type (RWTX or ROTX)
  ///
  /// @param tx The current active transaction
  ///
  /// @return A copy of the key that was found
  template <class TX> std::optional<K> front(TX &tx) {
    // Read head's next pointer in the data layer, abort if the read fails
    auto next_o = head->tower[0].next.get(tx, head);
    if (!next_o)
      return {};
    // Now we can just return the next node's key
    return next_o.value()->key;
  }

  /// Report if the skip list is empty
  ///
  /// @tparam TX the transaction type (RWTX or ROTX)
  ///
  /// @param tx The current active transaction
  ///
  /// @return True if the set is empty, false otherwise (none on abort)
  template <class TX> std::optional<bool> empty(TX &tx) {
    // Read head's next pointer in the data layer, abort if the read fails
    auto next_o = head->tower[0].next.get(tx, head);
    if (!next_o)
      return {};
    // Now we can just compare to the tail sentinel
    return next_o.value() == tail;
  }

  void dump(ROTX &ro) {
    auto curr = head->tower[0].next.get(ro, head).value();
    while (curr != tail) {
      std::cout << curr->key << "(" << curr->height << ")" << ", ";
      curr = curr->tower[0].next.get(ro, curr).value();
    }
    std::cout << "\n";
  }

private:
  /// Find the highest level that contains non-sentinel nodes.
  ///
  /// @param rw An open read/write transaction.
  ///
  /// @return The highest level that contains a non-sentinel node.  0 if the
  ///         skip list is empty.  {} on abort.
  std::optional<int> skip_empty_levels(RWTX &rw) {
    // Scan the head sentinel's tower to find the highest non-tail level.
    for (int i = NUM_INDEX_LAYERS; i > 0; --i) {
      auto t_o = head->tower[i].next.get(rw, head);
      if (!t_o)
        return {}; // ABORT!
      if (t_o.value() != tail)
        return i;
    }
    // Default: The list is empty, so just return level 0.
    return 0;
  }

  /// Skip forward from `start`, considering only tower level `level`,
  /// stopping at the furthest-right node with a key < the search key.
  ///
  /// NB: We know that we'll never find a key == the search key
  ///
  /// @param rw     An open read/write transaction
  /// @param key    The key for which we are doing a predecessor query.
  /// @param start  The start position of this skip.
  /// @param level  The tower level to consider
  ///
  /// @return The node (possibly head) that was found, or NONE on abort
  std::optional<sl_node_t *> skip_forward(RWTX &rw, K const key,
                                          sl_node_t *const start,
                                          uint64_t const level) {
    sl_node_t *curr = start;
    while (true) {
      // Try to read curr's successor at `level`
      auto next_o = curr->tower[level].next.get(rw, curr);
      if (!next_o)
        return {};
      auto next = next_o.value();

      // If we're at tail, return curr, even if curr is head
      if (next == tail)
        return curr;
      
      // Next isn't tail, so its key is valid. It CAN'T match. Stop if too big (or small, dependent on ordering)
      if (ORDER == order_t::INCREASING) {
        if (next->key > key)
          return curr;
      } else {
        if (next->key < key) //!
          return curr;
      }
      
      curr = next;
    }
  }

  /// Skip from the head sentinel to the greatest key *in each layer*
  /// with a key <= search key.  At any level, it can return the head data
  /// sentinel, but never the tail sentinel.
  ///
  /// @tparam preds_t The type of the elements of a predecessor array
  ///
  /// @param rw       An open read/write transaction
  /// @param key      The key for which we are doing a predecessor query..
  /// @param preds    The array of predecessors to be populated by this method.
  /// @param height   The insert height, indicating how many preds must be saved
  ///
  /// @return True on success; false on transaction abort.
  template <typename preds_t>
  [[nodiscard]] bool skip_insert(RWTX &rw, K const &key, preds_t &preds,
                                 int const height) {
    auto level_o = skip_empty_levels(rw);
    if (!level_o)
      return false; // ABORT!
    int level = level_o.value();

    // For any skipped layers whose pred we'll need, set them to head
    std::fill_n(std::begin(preds) + level + 1, height - level, head);

    // Now start traversing from the head, in the first nonempty level
    sl_node_t *curr = head;
    // Skip over and down layers above the insert height
    while (level > height) {
      auto curr_o = skip_forward(rw, key, curr, level);
      if (!curr_o)
        return false; // ABORT!
      curr = curr_o.value();
      --level;
    }

    // [mfs] Is there any harm in combining these loops?

    // Skip over and down layers below the insert height while saving preds
    while (level >= 0) {
      auto curr_o = skip_forward(rw, key, curr, level);
      if (!curr_o)
        return false; // ABORT!
      curr = curr_o.value();
      preds[level] = curr;
      --level;
    }

    // We've populated preds, so we're done :)
    return true;
  }

  /// Stitch a new node into the skip list.
  ///
  /// @param rw     An open read/write transaction.
  /// @param preds  An array of predecessors of the new node at each level.
  /// @param node   The new node
  ///
  /// @return True on success; false on transaction abort.
  template <typename preds_t>
  [[nodiscard]] bool sl_stitch(RWTX &rw, preds_t &preds, sl_node_t *node) {
    // Insert from bottom up
    uint8_t const target_height = node->height;
    for (uint8_t level = 0; level <= target_height; ++level) {
      // Get pred's successor at this level
      sl_node_t *pred = preds[level];
      auto succ_o = pred->tower[level].next.get(rw, pred);
      if (!succ_o)
        return false; // ABORT!
      // Stitch `node` in between pred and succ
      if (!node->tower[level].next.set(rw, node, succ_o.value()) ||
          !pred->tower[level].next.set(rw, pred, node))
        return false; // ABORT!
    }
    return true;
  }

  /// Generate a random level for a new node
  ///
  /// NB: This code has been verified to produce a nice geometric distribution
  ///     in constant time per call
  ///
  /// @param me The caller's OPTSTM operation
  ///
  /// @return a random number between 0 and NUM_INDEX_LAYERS, inclusive
  int randomLevel(OPTSTM *me) {
    // Get a random int between 0 and 0xFFFFFFFF
    int rr = me->rand();
    // Add 1 to it, then find the lowest nonzero bit.  This way, we never return
    // a zero for small integers, and the distribution is correct.
    int res = __builtin_ffs(rr + 1);
    // Now take one off of that, so that we return a zero-based integer
    res -= 1;
    // But if rr was 0xFFFFFFFF, we've got a problem, so coerce it back
    // Also, drop it down to within NUM_INDEX_LAYERS
    return (res < 0 || res > NUM_INDEX_LAYERS) ? NUM_INDEX_LAYERS : res;
  }
};
