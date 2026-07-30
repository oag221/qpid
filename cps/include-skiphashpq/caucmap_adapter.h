// 2024-11-20: This data structure is functional.  There are a few outstanding
// issues:
//
// - With a smarter API, we might not need get()?

#pragma once

#include <cstdint>
#include <functional>
#include <vector>
#include <sstream>
#include <iostream>
#include <optional>
#include <utility>

/// A straightforward non-resizable hashtable.  This map is designed to hold
/// key/collection pairs, which leads to a slightly different API.  See
/// dclist_omap for a reference implementation of `C`.
///
/// @param K      The type of the keys stored in this map 
/// @param C      The type of the collections stored in this map //! QD_T
/// @param OPTSTM The OPTSTM implementation
/// @param OMAP   An ordered map type to use as each bucket
///
/// NB: OMAP must be templated on <K, C, OPTSTM>
template <typename K, typename C, class OPTSTM, class OMAP>
class caucmap_adapter {
  using ROTX = typename OPTSTM::RO;
  using RWTX = typename OPTSTM::RW;

  OMAP **buckets;             // The OMAPs that act as the buckets in the table.
  const uint64_t num_buckets; // The number of buckets in the table.

public:
  /// Create a non-resizable hash table with the specified number of buckets.
  ///
  /// @param me  The operation that is constructing the table.
  /// @param cfg A configuration object with a `buckets` field
  template <typename config_t>
  caucmap_adapter(OPTSTM *me, config_t *cfg) : num_buckets(cfg->buckets) {
    buckets = (OMAP **)malloc(num_buckets * sizeof(OMAP *));
    // Fill the "buckets" vector with empty OMAPs
    for (unsigned int i = 0; i < num_buckets; ++i)
      buckets[i] = new OMAP(me, cfg);
  }

  ~caucmap_adapter() {
    for (unsigned int i = 0; i < num_buckets; ++i)
      delete buckets[i];
    free(buckets);
  }
  
private:
  std::hash<K> pre_hash;

  /// Get the index of the bucket where the provided key belongs
  ///
  /// @param me  The calling thread's descriptor
  /// @param key The key to hash.
  ///
  /// @return The hashed value of the key, modded by the number of buckets
  int hash(OPTSTM *me, const K key) {
    return me->hash(pre_hash(key)) % num_buckets;
  }

public:
  /// Search the data structure for a node with key `key`.  If not found, return
  /// false.  If found, return a reference with the associated collection
  ///
  /// @tparam TX The type of the active transaction (RO or RW)
  ///
  /// @param me  The calling thread's descriptor
  /// @param key The key to search
  /// @param val A ref parameter for returning key's value, if found
  ///
  /// @return A reference to the collection if found, nullptr if not, and NONE
  ///         on abort
  template <class TX> std::optional<C *> get_extract(TX &tx, const K &key) {
    return buckets[hash(tx.OP(), key)]->get_extract(tx, key);
  }

  template <class TX> std::optional<C *> get_ins(TX &tx, const K &key) {
    return buckets[hash(tx.OP(), key)]->get_ins(tx, key);
  }

  /// "Upsert" an empty collection for `key` and return a reference to it
  ///
  /// @param rw  A writing transactional context
  /// @param key The key for the mapping to upsert
  ///
  /// @return A reference to the collection for `key`, or NONE on abort
  std::optional<C *> make_collection(RWTX &rw, const K &key) {
    return buckets[hash(rw.OP(), key)]->make_collection(rw, key);
  }

  /// Remove the node associated with `key`
  ///
  /// @param rw  A writing transactional context
  /// @param key The key for the mapping to eliminate
  ///
  /// @return True if the key was found and removed, false otherwise, NONE on
  ///         abort
  std::optional<bool> remove(RWTX &rw, const K &key) {
    return buckets[hash(rw.OP(), key)]->remove(rw, key);
  }

  std::pair<long,long> dump(ROTX &ro, bool print=false) {
    long num_elems = 0;
    long keysum = 0;
    //int print_rate = 1;
    for (unsigned i = 0; i < num_buckets; ++i) {
      
      std::pair<int,long> ret = buckets[i]->dump(ro, print);
      num_elems += ret.first;
      keysum += ret.second;
      // if (ret.first > 0) {
      //   if (print_rate) {
      //     print_rate--;
      //   } else {
      //     std::cout << "bucket-" << i << ": total num_elems=" << num_elems << ", total keysum=" << keysum << "\n";
      //     print_rate = 1;
      //   }
      // }
    }
    return std::make_pair(num_elems, keysum);
  }

  std::vector<std::pair<K,long>> dump_ht() {
    long num_elems = 0;
    long keysum = 0;
    //int print_rate = 1;
    std::vector<std::pair<K,long>> ret;
    for (unsigned i = 0; i < num_buckets; ++i) {
      auto ret_bucket = buckets[i]->dump_ht();
      ret.insert(ret.end(), ret_bucket.begin(), ret_bucket.end());
    }
    return ret;
  }
};
