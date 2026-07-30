#include "smq.h"

#include <string>
#include <optional>
#include <utility>

template<typename T,
        typename J,
         //typename Comparer,
         size_t StealProb,
         size_t StealBatchSize,
         bool Concurrent>
void smq_ns::StealingMultiQueue<T, J, StealProb, StealBatchSize, Concurrent>::init_thread(int tid) {
    t_tid = tid;
}

template<typename T,
        typename J,
         //typename Comparer,
         size_t StealProb,
         size_t StealBatchSize,
         bool Concurrent>
std::optional<T> smq_ns::StealingMultiQueue<T, J, StealProb, StealBatchSize, Concurrent>::del_wrapper() {
    return extract_min();
}

template<typename T,
        typename J,
         //typename Comparer,
         size_t StealProb,
         size_t StealBatchSize,
         bool Concurrent>
std::optional<T> smq_ns::StealingMultiQueue<T, J, StealProb, StealBatchSize, Concurrent>::extract_min() {
    auto& buffer = stealBuffers[t_tid].stealBuf;
    if (!buffer.empty()) {
        auto val = buffer.back();
        buffer.pop_back();
        heaps[t_tid].heap.fillBufferIfStolen();
        return val;
    }
    // rand == 0 -- try to steal
    // otherwise, pop locally
    if (nQ > 1 && random() % StealProb == 0) {
        Galois_SMQ::optional<T> stolen = trySteal();
        if (stolen.is_initialized()) {
            return stolen.get();
        } 
    }
    auto minVal = heaps[t_tid].heap.extractMin();
    if (minVal.is_initialized()) {
        return minVal.get();
    }

    // Our heap is empty.
    if (nQ == 1) {
        return {};
    }
    Galois_SMQ::optional<T> res = trySteal();
    if (res.is_initialized()) {
        return res.get();
    }
    return {};
}

template<typename T,
         //typename Comparer,
         typename J,
         size_t StealProb,
         size_t StealBatchSize,
         bool Concurrent>
#ifdef PAGE_RANK
void smq_ns::StealingMultiQueue<T, J, StealProb, StealBatchSize, Concurrent>::insert(float key, uint32_t value)
#elif defined(RBP)
void smq_ns::StealingMultiQueue<T, J, StealProb, StealBatchSize, Concurrent>::insert(uint32_t key, J value)
#else
void smq_ns::StealingMultiQueue<T, J, StealProb, StealBatchSize, Concurrent>::insert(uint32_t key, uint32_t value)
#endif
{
   push(std::make_pair(key, value));
}

template<typename T,
         typename J,
         //typename Comparer,
         size_t StealProb,
         size_t StealBatchSize,
         bool Concurrent>
void smq_ns::StealingMultiQueue<T, J, StealProb, StealBatchSize, Concurrent>::push(T elem) {
    Heap* heap = &heaps[t_tid].heap;
    heap->pushLocally(elem);
    heap->fillBufferIfStolen();
  }

template<typename T,
         typename J,
         //typename Comparer,
         size_t StealProb,
         size_t StealBatchSize,
         bool Concurrent>
long smq_ns::StealingMultiQueue<T, J, StealProb, StealBatchSize, Concurrent>::debugKeySum() {
    long sum = 0;
    for (int i = 0; i < nQ; i++) {
        Heap *randH = &heaps[i].heap;
        sum += randH->get_keysum();
    }
    return sum;
}

template<typename T,
        typename J,
         //typename Comparer,
         size_t StealProb,
         size_t StealBatchSize,
         bool Concurrent>
bool smq_ns::StealingMultiQueue<T, J, StealProb, StealBatchSize, Concurrent>::getValidated() {
    return true;
}

template<typename T,
        typename J,
         //typename Comparer,
         size_t StealProb,
         size_t StealBatchSize,
         bool Concurrent>
long smq_ns::StealingMultiQueue<T, J, StealProb, StealBatchSize, Concurrent>::getSize() {
    long size = 0;
    for (int i = 0; i < nQ; i++) {
        Heap *randH = &heaps[i].heap;
        size += randH->get_size();
    }
    return size;
}


template<typename T,
        typename J,
         //typename Comparer,
         size_t StealProb,
         size_t StealBatchSize,
         bool Concurrent>
std::string smq_ns::StealingMultiQueue<T, J, StealProb, StealBatchSize, Concurrent>::getSizeString() {
    return std::to_string(getSize());
}
