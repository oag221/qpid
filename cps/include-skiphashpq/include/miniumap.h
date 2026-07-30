// Last Review: Needs Review

// needs review

#pragma once

#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iterator>

/// miniumap combines a hash-based index with a vector, so that we can quickly
/// find elements, and still iterate through the collection efficiently.
/// miniumap also has O(1) clearing.
///
/// NB: miniumap is based on the RedoLog from our STM libraries
///
/// @param K: The key type for entries in a miniumap
/// @param V: The value type for entries in a miniumap
template <typename K, typename V> class miniumap {
  /// The hash index consists of a version (for fast clearing), a key, and an
  /// index into the vector.
  struct index_t {
    size_t version; // If /version/ == /miniumap.version/ this index_t is valid
    K key;          // The key that is hashed at this index
    size_t index;   // An index into the vector

    /// Constructor: Note that version 0 is not allowed in the miniumap
    index_t() : version(0), key(0), index(0) {}
  };

  /// The pair that we keep in the vector
  struct vec_t {
    K key; // We duplicate the key, for fast rehashing
    V val; // The value with that key
  };

  /// number of static probes before we resize the list
  static const int SPILL_FACTOR = 3;

  /// The "hashtable" of the miniumap
  index_t *index;

  /// Size of hashtable
  size_t ilength;

  /// For fast-clearing of the hash table
  size_t version;

  /// used by the hash function
  size_t shift;

  /// The "vector" of the miniumap
  vec_t *vec;

  /// Capacity of the vector
  size_t vector_capacity;

  /// Current # elements in vector
  size_t vector_size;

  /// This hash function is straight from CLRS (that's where the magic constant
  /// comes from).
  size_t hash(K k) const {
    uint64_t key = (uintptr_t)k;
    static const unsigned long long s = 2654435769ull;
    const unsigned long long r = ((unsigned long long)key) * s;
    return (size_t)((r & 0xFFFFFFFF) >> shift);
  }

  /// Double the size of the index. This *does not* do anything as far as
  /// actually doing memory allocation. Callers should delete[] the index table,
  /// increment the table size, and then reallocate it.
  size_t doubleIndexLength() {
    assert(shift != 0 && "ERROR: minivector index is too large");
    shift -= 1;
    ilength = 1 << (8 * sizeof(uint32_t) - shift);
    return ilength;
  }

  /// Increase the size of the hash and rehash everything
  __attribute__((noinline)) void rebuild() {
    assert(version != 0 && "ERROR: the version should *never* be 0");

    // double the index size
    delete[] index;
    index = new index_t[doubleIndexLength()];

    // rehash the elements
    for (size_t i = 0; i < vector_size; ++i) {
      // search for the next available slot
      size_t h = hash(vec[i].key);
      while (index[h].version == version)
        h = (h + 1) % ilength;

      index[h].key = vec[i].key;
      index[h].version = version;
      index[h].index = i;
    }
  }

  /// Double the size of the vector if/when it becomes full
  __attribute__((noinline)) void resize() {
    vec_t *temp = vec;
    vector_capacity *= 2;
    vec = (vec_t *)malloc(vector_capacity * sizeof(vec_t));
    memcpy(vec, temp, sizeof(vec_t) * vector_size);
    free(temp);
  }

  /// zero the hash on version# overflow... highly unlikely
  __attribute__((noinline)) void reset_internal() {
    memset(index, 0, sizeof(index_t) * ilength);
    version = 1;
  }

public:
  /// Construct a miniumap by providing an initial capacity (default 64)
  miniumap(const size_t initial_capacity = 64)
      : index(nullptr), ilength(0), version(1), shift(8 * sizeof(uint32_t)),
        vec(nullptr), vector_capacity(initial_capacity), vector_size(0) {
    // Find a good index length for the initial capacity of the list.
    while (ilength < SPILL_FACTOR * initial_capacity)
      doubleIndexLength();
    index = new index_t[ilength];
    vec = (vec_t *)malloc(vector_capacity * sizeof(vec_t));
  }

  /// Reclaim the dynamically allocated parts of a miniumap when we destroy it
  ~miniumap() {
    delete[] index;
    free(vec);
  }

  /// Find the vector index for the given key, or -1 on failure
  int lookup(K key) {
    size_t h = hash(key);
    while (index[h].version == version) {
      if (index[h].key != key) {
        // use linear probing... given SPILL_FACTOR, we never wrap around
        h = (h + 1) % ilength;
        continue;
      }
      return index[h].index;
    }
    return -1;
  }

  /// Fast check if the miniumap is empty
  bool empty() const { return vector_size == 0; }

  /// reserve is effectively the first half of an "upsert".  It finds the vector
  /// entry into which a key should go, or makes that vector entry
  ///
  /// NB: we expect key's low bits to be masked to zero
  int reserve(K key) {
    //  Find the slot that this key should hash to. If it is valid,
    //  return the index. If we find an unused slot then it's a new
    //  insertion.
    size_t h = hash(key);
    while (index[h].version == version) {
      if (index[h].key == key) {
        return index[h].index;
      }
      // keep probing...
      h = (h + 1) % ilength;
    }

    // at this point, h is a valid insertion point
    index[h].key = key;
    index[h].version = version;
    index[h].index = vector_size;

    // update the next element pointer into the vector
    ++vector_size;

    // resize the vector if there's only one spot left
    if (__builtin_expect(vector_size == vector_capacity, false))
      resize();

    // if we reach our load-factor, rebuild
    if (__builtin_expect((vector_size * SPILL_FACTOR) >= ilength, false)) {
      rebuild();
      return reserve(key);
    }
    return index[h].index;
  }

  /// fast-clear the hash by bumping the version number
  void clear() {
    vector_size = 0;
    version += 1;
    // check overflow
    if (version != 0)
      return;
    reset_internal();
  }

  /// type-specialized code for inserting an key/value pair into the miniumap
  vec_t *insert(K key, V val) {
    auto found = find(key);
    if (found != vec + vector_size)
      return found;

    auto idx = reserve(key);
    vec[idx].key = key;
    vec[idx].val = val;
    return nullptr;
  }

  /// type-specialized code for looking up a value from the miniumap
  vec_t *find(K key) {
    // get slab target, see if it's valid
    int idx = lookup(key);
    if (idx == -1)
      return vec + vector_size; // not valid because it doesn't exist
    return &vec[idx];
  }

  /// Iterator type
  using iterator = vec_t *;

  /// Reverse iterator type
  using reverse_iterator = std::reverse_iterator<iterator>;

  /// Get an iterator to the start of the array
  iterator begin() const { return vec; }

  /// Get an iterator to one past the end of the array
  iterator end() const { return vec + vector_size; }

  /// Get the starting point for a reverse iterator
  reverse_iterator rbegin() { return reverse_iterator(end()); }

  /// Get the ending point for a reverse iterator
  reverse_iterator rend() { return reverse_iterator(begin()); }
};
