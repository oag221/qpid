#pragma once

#ifndef CACHE_LINE_SIZE
constexpr std::size_t CACHE_LINE_SIZE = 64;
#endif

template <typename P, typename J, class OPTSTM>
class chunk_queue; // Forward declaration

template <typename P, typename J, class OPTSTM>
struct alignas(CACHE_LINE_SIZE) chunk_pool {
    using RWTX = typename OPTSTM::RW;
    using chunk_t = typename chunk_queue<P,J,OPTSTM>::q_node_t;

    const int CHUNKSIZE;
    const int INITIAL_RESERVE;
    const int INITIAL_CHUNKS;
    std::vector<chunk_t*> chunks;

  #ifdef PROFILING
    bool accessed_pool = false;
    bool used_pool = false;
  #endif

  public:
    chunk_pool(int chunksize, int initial_reserve=1000, int initial_chunks=0, OPTSTM *me=nullptr) : CHUNKSIZE(chunksize), INITIAL_RESERVE(initial_reserve), INITIAL_CHUNKS(initial_chunks) {
        // init the pool - logic in a function bc init may be called again, i.e., after call to constructor
        pool_init(me);
    }

  #ifdef PROFILING
  
    void reset_profiling_fields() {
        accessed_pool = false;
        used_pool = false;
    }

    /// @return nullopt if pool not accessed, true if accessed and pool used, false if accessed and pool not used
    std::optional<bool> check_prof_fields() {
        std::optional<bool> ret = {true};
        if (accessed_pool) {
            if (!used_pool) ret = {false};
        } else {
            ret = std::nullopt;
        }
        // reset fields before returning
        reset_profiling_fields();
        return ret;
    }

    void set_prof_fields(bool accessed, bool used) {
        accessed_pool = accessed;
        used_pool = used;
    }

  #endif

    /// Initialize (can also re-init) the pool
    ///
    /// @param me The caller's thread context (for STM)
    void pool_init(OPTSTM *me) {
        if (INITIAL_RESERVE) {
            chunks.reserve(INITIAL_RESERVE);
        }

        if (INITIAL_CHUNKS) {
            if (!me) std::terminate();

            // in case calling a second time (after prefilling)
            int cur_size = chunks.size();

            RWTX rw(me);
            for (int i = cur_size; i < INITIAL_CHUNKS; i++) {
                chunks.push_back(chunk_t::make_node(rw, CHUNKSIZE));
            }
            if (!me->try_end_rw())
                std::terminate();
        }
    }

    /// Get the last chunk from the vector of chunks
    ///
    /// NOTE: does not remove the chunk, just returns it
    ///
    /// @return nullptr if vector is empty, else a pointer to the last chunk
    chunk_t* pool_get_chunk() {
        if (chunks.empty()) return nullptr;
        return (chunk_t*)chunks.back();
    }

    /// Removes the last chunk from the vector of chunks, if non-empty
    void pool_remove_chunk() {
        if (chunks.empty()) {
            std::cout << "WARNING: requested to remove a chunk from pool, but pool is empty.\n";
            return;
        } 
        chunks.pop_back();
    }

    /// Insert a chunk to the end of the vector
    void pool_insert_chunk(chunk_t* ins_chunk) {
        chunks.push_back(ins_chunk);
    }
};

