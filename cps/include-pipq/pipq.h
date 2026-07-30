/**
 * C++ implementation of PIPQ priority queue
 * 
 * 
 */
#include <string>
#include <atomic>
#include <sstream>
#include <set>
#include <iostream>
#include <optional>
#include <numa.h>

#include "harris_impl.h"
#include "debugprinting.h"

#ifndef PQ_H
#define	PQ_H

#define EMPTY -1

#define PARENT(i)      ((i - 1) / 2)
#define LEFT_CHILD(i)  ((2 * i) + 1)
#define RIGHT_CHILD(i) ((2 * i) + 2)

// configure for machine
#define NUMA_ZONES 4
#define NUMA_ZONE_PHYS_CORES 24
#define NUMA_ZONE_THREADS 48

#define NODE_0 0
#define NODE_1 1
#define NODE_2 2
#define NODE_3 3

//#define ALIGN_SIZE  64
#define CACHE_ALIGN __attribute__((aligned(ALIGN_SIZE)))

#define synchFullFence() asm volatile("mfence" ::: "memory")

using namespace std;

namespace pq_ns {

    template <class V>
    class pipq {
    public:
        /*
            The following are initiallizations for the variables and datastructures 
            that will be used for the different sockets.
        */

        struct PQ_Node {
            int key;
            V value;
        };

        // the heap list
        struct __attribute__((__packed__)) HeapList {
            PQ_Node* heapList; // init to HEAP_LIST_SIZE, the size of each array
            HeapList* next; // init to NULL, if the heapList becomes full, then we allocate another of the same size
            HeapList* prev;
            char padding[(ALIGN_SIZE - (sizeof(PQ_Node*) + 2*sizeof(HeapList*)))];
        };

        //Heap struct - worker
        struct __attribute__((__packed__)) PQ_Heap {
            int size;
            HeapList* pq_ptr; // the heap - a vector of PQ_NODE's 
            volatile long* lock;
            char padding[(ALIGN_SIZE - (sizeof(int) + sizeof(HeapList*) + sizeof(long*)))];
        };

        //sizeof 8
        struct AnnounceStruct {
            volatile int status; // active request (1) or not (0)
            volatile int detected;
            volatile int key; // value to insert, OR return value (if needed)
            volatile V value; // value to insert, OR return value (if needed)
        };

        struct __attribute__((__packed__)) CounterSlot {
            volatile int count;
            char padding[(ALIGN_SIZE - (sizeof(volatile int)))];
        };

        struct __attribute__((__packed__)) DelMinCntr {
            volatile int num;
            volatile long sum;
            char padding[(ALIGN_SIZE - (sizeof(volatile int) + sizeof(volatile long)))];
        };
        DelMinCntr* delmin_cntr_0;
        DelMinCntr* delmin_cntr_1;
        DelMinCntr* delmin_cntr_2;
        DelMinCntr* delmin_cntr_3;
        inline static thread_local DelMinCntr* t_delmin_cntr;
        
        // thread local heaps - one per NUMA zone, with a getter for helping
        PQ_Heap** heap_0 CACHE_ALIGN;
        PQ_Heap** heap_1 CACHE_ALIGN;
        PQ_Heap** heap_2 CACHE_ALIGN;
        PQ_Heap** heap_3 CACHE_ALIGN;

        //pq_t *linden_instance CACHE_ALIGN;
        CounterSlot* lead_counters_0 CACHE_ALIGN;
        CounterSlot* lead_counters_1 CACHE_ALIGN;
        CounterSlot* lead_counters_2 CACHE_ALIGN;
        CounterSlot* lead_counters_3 CACHE_ALIGN;
        inline static thread_local CounterSlot* t_lead_counters;

        intset_t* leader_set;

        // coordinator structure - smallest element from each socket
        volatile long* coord_lock;
        volatile long long *repeat_keys;

        volatile long* compete_coord_0;
        volatile long* compete_coord_1;
        volatile long* compete_coord_2;
        volatile long* compete_coord_3;

        AnnounceStruct *announce_coord_0 CACHE_ALIGN, *announce_coord_1 CACHE_ALIGN, *announce_coord_2 CACHE_ALIGN, *announce_coord_3 CACHE_ALIGN;

        // all of the following are declared as thread local - a thread only accesses the local copy
        /*
            _group          : id of the NUMA zone that the thread belongs to
            _tid            : thread id of the current thread
            _cpu_id         : the thread local cpu_id
            _t_local_heap   : pointer to the thread local heap
        */
        int *counter_0, *counter_1, *counter_2, *counter_3;
        inline static thread_local int t_group, t_tid, t_idx, t_num_workers;
        inline static thread_local PQ_Heap *t_local_heap;
        inline static thread_local AnnounceStruct *announce_coord;
        inline static thread_local volatile long* t_compete_coord_lock;
        inline static thread_local bool* t_exit;
        inline static thread_local LeaderLargest* t_largest_in_leader;

        /*
            Copy per NUMA zone (but all are identical) to minimize cross-NUMA traffic during helping
        */
        int *thread_mappings_0, *thread_mappings_1, *thread_mappings_2, *thread_mappings_3;

        LeaderLargest *largest_in_leader_0, *largest_in_leader_1, *largest_in_leader_2, *largest_in_leader_3;

        bool *active_numa_zones_0;
        bool *active_numa_zones_1;
        bool *active_numa_zones_2;
        bool *active_numa_zones_3;

        pthread_barrier_t WaitForAll;

        struct __attribute__((__packed__)) DebugCounterSlot {
            volatile long count;
            char padding[(ALIGN_SIZE - (sizeof(volatile long)))];
        };

        DebugCounterSlot *num_moves_0, *num_moves_1, *num_moves_2, *num_moves_3;
        DebugCounterSlot *num_ins_0, *num_ins_1, *num_ins_2, *num_ins_3;
        DebugCounterSlot *num_fastpath_0, *num_fastpath_1, *num_fastpath_2, *num_fastpath_3;
        inline static thread_local DebugCounterSlot *t_num_moves, *t_num_ins, *t_num_fastpath;

        long long keySum;
        long long finalSize;
        long numMoves;
        long numTraversed;
        long numFastPath;
        long numIns;
        long numUpsert;
        long numCoordUpsert;
        bool validated;
        bool validate_run = false;
        float avg_delmin_ops;

        const int HEAP_LIST_SIZE;
        // const int LEADER_BUFFER_CAP;
        // const int LEADER_BUFFER_IDEAL_SIZE;
        const int TOTAL_THREADS;
        const int COUNTER_THRESHOLD;
        const int COUNTER_MAX;
        const int MAX_OFFSET;
        const int* PINNING_ORDER;

        // constructor
        pipq(int hls, /*int lead_buf_capacity, int lead_buf_ideal,*/ int tds, int counter_ths, int counter_max, const int* pin_order, int offset=32) :
                HEAP_LIST_SIZE(hls),
                // LEADER_BUFFER_CAP(lead_buf_capacity),
                // LEADER_BUFFER_IDEAL_SIZE(lead_buf_ideal),
                TOTAL_THREADS(tds),
                COUNTER_THRESHOLD(counter_ths),
                COUNTER_MAX(counter_max),
                MAX_OFFSET(offset),
                PINNING_ORDER(pin_order) {
                
            pthread_barrier_init(&WaitForAll, NULL, tds);
            keySum = 0;
            finalSize = 0;
            numMoves = 0;
            numTraversed = 0;
            numFastPath = 0;
            numIns = 0;
            numUpsert = 0;
            numCoordUpsert = 0;
            validated = true;
        }

        void *numa_alloc_local_aligned(size_t size) { //alligned allocation for the NUMA protocol
            void *p     = numa_alloc_local(size); // note: this is a built in function which allocates "size" bytes on the local node
            long plong  = (long)p;
            plong       += ALIGN_SIZE;
            plong       &= ~(ALIGN_SIZE - 1);
            p           = (void *)plong;
            return p;
        }


        /////////////////////////////////////////////////////////////////
        /////////////////////////////////////////////////////////////////
        // GETTERS for NUMA-local structures
        /////////////////////////////////////////////////////////////////
        /////////////////////////////////////////////////////////////////

        // note: this is specific to Luigi
        int get_group(int cpu_id) {
            return numa_node_of_cpu(cpu_id);
        }

        // return the idx of the corresponding tid
        int get_thread_mapping(int group, int tid) {
            switch(group) {
                case 0: return thread_mappings_0[tid]; break;
                case 1: return thread_mappings_1[tid]; break;
                case 2: return thread_mappings_2[tid]; break;
                case 3: return thread_mappings_3[tid]; break;
                default: return -1; break; // should never get here
            }
        }

        PQ_Heap* get_heap_mapping(int idx, int group) {
            PQ_Heap** heap = get_worker_heap(group);
            return heap[idx];
        }

        CounterSlot* get_counters(int group) {
            switch(group) {
                case NODE_0: return lead_counters_0; break;
                case NODE_1: return lead_counters_1; break;
                case NODE_2: return lead_counters_2; break;
                case NODE_3: return lead_counters_3; break;
                default: COUTATOMIC("SHOULD NOT BE HERE (get_counters())\n"); exit(0); break;
            }
        }

        CounterSlot* get_counters(int group, int idx) {
            switch(group) {
                case NODE_0: return &(lead_counters_0[idx]); break;
                case NODE_1: return &(lead_counters_1[idx]); break;
                case NODE_2: return &(lead_counters_2[idx]); break;
                case NODE_3: return &(lead_counters_3[idx]); break;
                default: COUTATOMIC("SHOULD NOT BE HERE (get_counters())\n"); exit(0); break;
            }
        }

        // for main method to retrieve
        int get_tidx() {
            return t_idx; 
        }

        // for main method to retrieve
        int get_local_numa_workers() {
            return t_num_workers;
        }

        // get the proper item based on the group
        PQ_Heap** get_worker_heap(int group) {
            switch (group) {
                case 0: return heap_0; break;
                case 1: return heap_1; break;
                case 2: return heap_2; break;
                case 3: return heap_3; break;
                default: return NULL; break;
            }
        }

        LeaderLargest* get_last_ptr(int idx, int zone) {
            switch (zone) {
                case 0: return &(largest_in_leader_0[idx]); break;
                case 1: return &(largest_in_leader_1[idx]); break;
                case 2: return &(largest_in_leader_2[idx]); break;
                case 3: return &(largest_in_leader_3[idx]); break;
                default: COUTATOMIC("SHOULD NOT BE HERE (get_last_ptr())\n"); exit(0); break;
            }
        }

        AnnounceStruct* get_announce_coord(int group) {
            switch(group) {
                case NODE_0: return announce_coord_0; break;
                case NODE_1: return announce_coord_1; break;
                case NODE_2: return announce_coord_2; break;
                case NODE_3: return announce_coord_3; break;
                default: COUTATOMIC("SHOULD NOT BE HERE (get_announce_coord())\n"); exit(0); break;
            }
        }

        bool check_active_zone(int curr_group, int check_group) {
            switch(curr_group) {
                case 0: return active_numa_zones_0[check_group]; break;
                case 1: return active_numa_zones_1[check_group]; break;
                case 2: return active_numa_zones_2[check_group]; break;
                case 3: return active_numa_zones_3[check_group]; break;
                default: return -1; break; // should never get here
            }
        }

        int get_numa_workers(int calling_group, int group) {
            switch(calling_group) {
                case 0: return counter_0[group]; break;
                case 1: return counter_1[group]; break;
                case 2: return counter_2[group]; break;
                case 3: return counter_3[group]; break;
                default: return -1; break;
            }
        }

        void clearCounters() {
            for (int i = 0; i < TOTAL_THREADS; i++) {
                int cpu_id = PINNING_ORDER[i];
                int group = get_group(cpu_id);
                int idx = get_thread_mapping(group, i);
                switch(group) {
                    case 0:
                        num_moves_0[idx].count = 0;
                        num_ins_0[idx].count = 0;
                        num_fastpath_0[idx].count = 0;
                        break;
                    case 1:
                        num_moves_1[idx].count = 0;
                        num_ins_1[idx].count = 0;
                        num_fastpath_1[idx].count = 0;
                        break;
                    case 2:
                        num_moves_2[idx].count = 0;
                        num_ins_2[idx].count = 0;
                        num_fastpath_2[idx].count = 0;
                        break;
                    case 3:
                        num_moves_3[idx].count = 0;
                        num_ins_3[idx].count = 0;
                        num_fastpath_3[idx].count = 0;
                        break;
                }
            }
        }

        long getTotalMoves() {
            long sum = 0;
            for (int i = 0; i < TOTAL_THREADS; i++) {
                int cpu_id = PINNING_ORDER[i]; // !NOTE: assumes threads are binded w no hyperthreading, numa zone by numa zone (0 -> 3)
                int group = get_group(cpu_id);
                int idx = get_thread_mapping(group, i);

                switch(group) {
                    case 0: sum += num_moves_0[idx].count; break;
                    case 1: sum += num_moves_1[idx].count; break;
                    case 2: sum += num_moves_2[idx].count; break;
                    case 3: sum += num_moves_3[idx].count; break;
                }
            }
            return sum;
        }

        long getTotalFastPath() {
            long sum = 0;
            for (int i = 0; i < TOTAL_THREADS; i++) {
                int cpu_id = PINNING_ORDER[i]; // !NOTE: assumes threads are binded w no hyperthreading, numa zone by numa zone (0 -> 3)
                int group = get_group(cpu_id);
                int idx = get_thread_mapping(group, i);

                switch(group) {
                    case 0: sum += num_fastpath_0[idx].count; break;
                    case 1: sum += num_fastpath_1[idx].count; break;
                    case 2: sum += num_fastpath_2[idx].count; break;
                    case 3: sum += num_fastpath_3[idx].count; break;
                }
            }
            return sum;
        }

        long getTotalInsertUp() {
            long sum = 0;
            for (int i = 0; i < TOTAL_THREADS; i++) {
                int cpu_id = PINNING_ORDER[i]; // !NOTE: assumes threads are binded w no hyperthreading, numa zone by numa zone (0 -> 3)
                int group = get_group(cpu_id);
                int idx = get_thread_mapping(group, i);

                switch(group) {
                    case 0: sum += num_ins_0[idx].count; break;
                    case 1: sum += num_ins_1[idx].count; break;
                    case 2: sum += num_ins_2[idx].count; break;
                    case 3: sum += num_ins_3[idx].count; break;
                }
            }
            return sum;
        }

        long long getSize() {
            long long size = 0;

            size += set_size(leader_set);
            long long leader_size = size;
            // // get sizes from thread-local heaps
            for (int i = 0; i < TOTAL_THREADS; i++) {
                int cpu_id = PINNING_ORDER[i];
                int group = get_group(cpu_id);
                int idx = get_thread_mapping(group, i);
                PQ_Heap* heap = get_heap_mapping(idx, group);
                size += heap->size;
            }
            long long worker_size = size - leader_size;

            COUTATOMIC("[" << t_idx << "] Sizes:\n\t" << "W: " << worker_size << "\n\tL: " << leader_size <<  endl);
            return size;
        }

        void validate_insertion_ordering() {
            COUTATOMIC("\n\nVALIDATING insertion ordering...\n");
            validate_run = true;
            int last_key;
            bool invalid = false;
            int num_incorrect = 0;
            int cnt = 1;

            // COUTATOMIC("Counter values:\n");
            // // print counter values for reference
            // for (int i = 0; i < TOTAL_THREADS; i++) {
            //     int cpu_id = i/24 + 4*(i % 24);
            //     int group = get_group(cpu_id);
            //     int idx = get_thread_mapping(group, i);
            //     CounterSlot* slot = get_counters(group, idx);
            //     COUTATOMIC("tid=" << i << ": " << slot->count << "\n");
            // }
            // COUTATOMIC("\n");

            int* largest_leader = new int[96];
            
            num_incorrect += set_validate(leader_set, largest_leader);
            if (num_incorrect) {
                invalid = true;
            }

            for (int i = 0; i < TOTAL_THREADS; i++) {
                int cpu_id = PINNING_ORDER[i];
                int group = get_group(cpu_id);
                int idx = get_thread_mapping(group, i);
                PQ_Heap* worker = get_heap_mapping(idx, group);
                LeaderLargest* last_ptr = get_last_ptr(idx, group);

                last_key = largest_leader[group * 24 + idx];
                assert(last_key == last_ptr->largest_ptr->key);
                COUTATOMIC("\n[tid=" << i << "]\nlast_key (from leader): " << last_key << "\n");
                COUTATOMIC("largest_key_ptr = " << last_ptr->largest_ptr->key << "\n");

                //! only comparing the largest in leader to smallest from worker
                int first = 0; //!
                while (first == 0) { //!
                //while (worker->size > 0) {
                    int min;
                    std::optional<V> up_val = delete_min_worker(worker, &min);
                    if (last_key > min && min != EMPTY) {
                        COUTATOMIC("(cnt=" << cnt << ") ORDER INCORRECT!!! " << last_key << " -> " << min << "\n");
                        invalid = true;
                        num_incorrect++;
                    }
                    last_key = min;
                    cnt++;
                    first++; //!
                }
            }
            delete[] largest_leader;

            if (invalid) {
                validated = false;
                COUTATOMIC("Ordering NOT validated! Number incorrect: " << num_incorrect << " \n\n\n");
            } else {
                COUTATOMIC("Ordering validated!\n\n\n");
            }
        }

        long long getKeySum() {
            long long sum = 0;

            COUTATOMIC("calculating leader..." << endl);
            sum += set_keysum(leader_set);

            long long leader_sum = sum;
            COUTATOMIC("leader sum is: " << leader_sum << endl);
            
            COUTATOMIC(endl << "calculating worker..." << endl);
            for (int i = 0; i < TOTAL_THREADS; i++) {
                int cpu_id = PINNING_ORDER[i];
                int group = get_group(cpu_id);
                int idx = get_thread_mapping(group, i);
                PQ_Heap* heap = get_heap_mapping(idx, group);
                HeapList* list = heap->pq_ptr;

                int cnt = 1;
                for (int j = 0; j < heap->size; j++) {
                    if (j >= cnt * HEAP_LIST_SIZE) {
                        list = list->next;
                        cnt++;
                    }
                    int idx = j % HEAP_LIST_SIZE;
                    sum += (list->heapList[idx]).key;
                }
            }
            long long worker_sum = sum - leader_sum;
            COUTATOMIC("worker sum is: " << worker_sum << endl << endl);

            long long tot_repeats = 0;
            for (int i = 0; i < TOTAL_THREADS; i++) { 
                tot_repeats += repeat_keys[i];
            }
            if (tot_repeats > 0) {
                COUTATOMIC("repeat keys value: " << tot_repeats << endl);
            }

            sum += tot_repeats; //to match thread keysum -
            COUTATOMIC(endl << "TOTAL sum is: " << sum << endl << endl);
            return sum;
        }

        void sumCntDelminOps() {
            int num_delmin = 0;
            int sum_delmin = 0;
            for (int i = 0; i < get_numa_workers(0, NODE_0); i++) {
                num_delmin += delmin_cntr_0[i].num;
                sum_delmin += delmin_cntr_0[i].sum;
            }

            for (int i = 0; i < get_numa_workers(0, NODE_1); i++) {
                num_delmin += delmin_cntr_1[i].num;
                sum_delmin += delmin_cntr_1[i].sum;
            }

            for (int i = 0; i < get_numa_workers(0, NODE_2); i++) {
                num_delmin += delmin_cntr_2[i].num;
                sum_delmin += delmin_cntr_2[i].sum;
            }

            for (int i = 0; i < get_numa_workers(0, NODE_3); i++) {
                num_delmin += delmin_cntr_3[i].num;
                sum_delmin += delmin_cntr_3[i].sum;
            }

            float avg = 0;
            if (num_delmin > 0) {
                avg = (float)sum_delmin / (float)num_delmin;
            }
            avg_delmin_ops = avg;
            COUTATOMIC("\nAVERAGE number of ops per del-min call: " << avg << "\n");
        }

        float get_avg_delmin_ops() {
            return avg_delmin_ops;
        }

        string getSizeString() {
            return to_string(finalSize);
        }

        string getNumMoves() {
            return to_string(numMoves);
        }

        string getCoordUpsert() {
            return to_string(numCoordUpsert);
        }

        string getNumTraversed() {
            return to_string(numTraversed);
        }

        string getNumFastPath() {
            return to_string(numFastPath);
        }

        string getNumIns() {
            return to_string(numIns);
        }

        string getNumUps() {
            return to_string(numUpsert);
        }

        long long debugKeySum() {
            return keySum;
        }

        bool getValidated() {
            return validated;
        }
        
        void PQInit(); // initializes thread local heaps, active NUMA zones
        void init_thread(int tid); // initializes: t_tid, t_idx and t_group
        void HeapInit(PQ_Heap **Heap, int group, int heap_size); // called by PQInit to initialize t-local heap
        void Announce_allocation(AnnounceStruct **announce, int size, int group);
        
        void PQDeinit();
        void HeapDeinit(PQ_Heap **Heap);

        // helpers for worker heap methods
        bool getChildList(HeapList** heapList, int* count, int* c_idx, int size, int heapSize);
        bool getParentList(HeapList** heapList, int* count, int* p_idx, int heapSize);
        
        // insert methods
        bool push(int key, V value);
        void insert_worker(PQ_Heap *Heap, int K, V value);

        // delete-min methods
        //int hier_delete(V& val); // for sssp
        std::optional<std::pair<int, V>> extract_min();
        int hier_delete();
        void try_compete_coordinator();
        void try_become_coordinator();
        void Coordinate();
        void delete_min_leader(int idx);
        std::optional<V> delete_min_worker(PQ_Heap *Heap, int* key);

        // used by both insert and delete-min to help upsert elements to leader when needed
        void help_upsert();
    };
}

#endif	/* PQ_H */