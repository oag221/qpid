#include "smq.h"

#include <string>

template<typename T,
         //typename Comparer,
        //  size_t StealProb,
        //  size_t StealBatchSize,
         bool Concurrent>
void smq_ns::StealingMultiQueue<T, Concurrent>::initThread(int tid) {
    t_tid = tid;
}

template<typename T,
         //typename Comparer,
        //  size_t StealProb,
        //  size_t StealBatchSize,
         bool Concurrent>
std::optional<T> smq_ns::StealingMultiQueue<T, Concurrent>::del_wrapper() {
    return pop();
}

template<typename T,
         //typename Comparer,
        //  size_t StealProb,
        //  size_t StealBatchSize,
         bool Concurrent>
std::optional<T> smq_ns::StealingMultiQueue<T, Concurrent>::pop() {
    auto& buffer = stealBuffers[t_tid].stealBuf;
    if (!buffer.empty()) {
        auto val = buffer.back();
        buffer.pop_back();
        heaps[t_tid].heap.fillBufferIfStolen();
        return val;
    }
    // rand == 0 -- try to steal
    // otherwise, pop locally
    if (nQ > 1 && random() % STEAL_PROB == 0) {
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
        //  size_t StealProb,
        //  size_t StealBatchSize,
         bool Concurrent>
void smq_ns::StealingMultiQueue<T, Concurrent>::ins_wrapper(uint32_t key, uint32_t value) {
   push(std::make_pair(key, value));
}

template<typename T,
         //typename Comparer,
        //  size_t StealProb,
        //  size_t StealBatchSize,
         bool Concurrent>
void smq_ns::StealingMultiQueue<T, Concurrent>::push(T elem) {
    Heap* heap = &heaps[t_tid].heap;
    heap->pushLocally(elem);
    heap->fillBufferIfStolen();
  }

template<typename T,
         //typename Comparer,
        //  size_t StealProb,
        //  size_t StealBatchSize,
         bool Concurrent>
long smq_ns::StealingMultiQueue<T, Concurrent>::debugKeySum() {
    long sum = 0;
    for (int i = 0; i < nQ; i++) {
        Heap *randH = &heaps[i].heap;
        sum += randH->get_keysum();
    }
    return sum;
}

template<typename T,
         //typename Comparer,
        //  size_t StealProb,
        //  size_t StealBatchSize,
         bool Concurrent>
bool smq_ns::StealingMultiQueue<T, Concurrent>::getValidated() {
    return true;
}

template<typename T,
         //typename Comparer,
        //  size_t StealProb,
        //  size_t StealBatchSize,
         bool Concurrent>
long smq_ns::StealingMultiQueue<T, Concurrent>::getSize() {
    long size = 0;
    for (int i = 0; i < nQ; i++) {
        Heap *randH = &heaps[i].heap;
        size += randH->get_size();
    }
    return size;
}


template<typename T,
         //typename Comparer,
        //  size_t StealProb,
        //  size_t StealBatchSize,
         bool Concurrent>
std::string smq_ns::StealingMultiQueue<T, Concurrent>::getSizeString() {
    return std::to_string(getSize());
}
