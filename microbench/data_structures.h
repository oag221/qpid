#if defined(QPID)

    #include <include-skiphashpq/optstm2/eager_noext_c1.h>
    #include <include-skiphashpq/skiphash_pq_relaxed.h>
    #include "config.h"

    #define INIT_THREAD pq.init_thread(me, tid)
    #define INSERT_FUNC pq.insert(me, key, key)
  #if defined(QPID_STRICT)
    #define EXTRACT_FUNC pq.extract_min_strict(me)
  #else
    #define EXTRACT_FUNC pq.extract_min(me) //! std::optional<std::pair<P,J>>
  #endif

#elif defined(MBQ)

    #include <include/MultiBucketQueue.h>
    #include <include/MultiQueue.h>

    #define INIT_THREAD pq.initTID()
    #define INSERT_FUNC pq.push(key, key)
    #define EXTRACT_FUNC pq.popInternal() //! boost::optional<PQElement>

#elif defined(LINDEN)
    #include <include-linden/linden.h>
    #include "common/gc/ptst.h"

    #define INIT_THREAD
    #define INSERT_FUNC insert(pq, key, key)
    #define EXTRACT_FUNC extract_min(pq) //! std::optional<std::pair<pkey_t,pval_t>>

#elif defined(PIPQ)
    #include <include-pipq/pipq_impl.h>

    #define INIT_THREAD pq.init_thread(tid)
    #define INSERT_FUNC pq.push(key, key)
    #define EXTRACT_FUNC pq.extract_min() //! std::optional<std::pair<int, V>>

#elif defined(SPRAYLIST)
    #include "common/intset.h"

    #define INIT_THREAD seeds = seed_rand()
    #define INSERT_FUNC sl_add(pq, key, key) //fraser_insert(pq, key, key) // todo: try the other one
    #define EXTRACT_FUNC spray_delete_min_key(pq, data_item) //! std::optional<std::pair<slkey_t,val_t>>

  #elif defined(SMQ)
    #include <include-smq/smq_impl.h>

    #define INIT_THREAD pq.init_thread(tid);
    #define INSERT_FUNC pq.insert(key, key) //fraser_insert(pq, key, key) // todo: try the other one
    #define EXTRACT_FUNC pq.extract_min();
#endif