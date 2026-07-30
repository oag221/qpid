#!/bin/bash

#./build/rbp pipq ising 1000 1 1 1 1 1 1 1 1 1 1 1 
# ./build/rbp pipq ising 1000 96 1 1 1 1 1 1 1 1 1 1
# echo ""
# ./build/rbp pipq ising 1000 48 1 1 1 1 1 1 1 1 1 1
# echo ""
# ./build/rbp pipq ising 1000 24 1 1 1 1 1 1 1 1 1 1
# echo ""
# ./build/rbp pipq ising 1000 12 1 1 1 1 1 1 1 1 1 1

threads=(96)
lanes=128
chunksize=(128 512)
delta=(21)

for c in "${chunksize[@]}"; do
    for d in "${delta[@]}"; do
        for t in "${threads[@]}"; do
            if [[ $t == 1 ]]; then
                q=1
            else
                q=$lanes
            fi
            LD_PRELOAD=/usr/lib/x86_64-linux-gnu/libjemalloc.so.2 ./build/rbp skiphashpq-rbp ising 1000 $t $q 0 0 $d 0 0 0 $c 0 1 # batch-opt THEN strict-op
            echo ""
        done
    done
done

# LD_PRELOAD=/usr/lib/x86_64-linux-gnu/libjemalloc.so.2 ./build/rbp skiphashpq-rbp ising 1000 1 128 0 0 22 0 0 0 128 1 0

# LD_PRELOAD=/usr/lib/x86_64-linux-gnu/libjemalloc.so.2 ./build/rbp skiphashpq-rbp ising 1000 96 "$QUEUES" 1 1 "$DELTA" 64 0 0 "$CHUNKSIZE" 2>&1