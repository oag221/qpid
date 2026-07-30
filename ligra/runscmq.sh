#!/bin/bash

input_dir=../../inputs_pbbs/ag

graphs=(USA LJ TW HYPERH)

stickinessbucket=(8 4 8 1)
stickinessheap=(8 4 8 1)
batches1=(256 256 32 256)
batches2=(256 256 256 256)

# MQBucket
for i in {0..3}
do
	echo "Results" > results_sc_48_${graphs[i]}_MQBucket.log
	./SetCover_MQ -rounds 10 -delta 0 -threads 48 -queues 192 -buckets 64 -stick ${stickinessbucket[i]} -batch1 256 -batch2 256 -type MQBucket -prefetch ${input_dir}/${graphs[i]}.ag  >> results_sc_48_${graphs[i]}_MQBucket.log
done

for i in {0..3}
do
        echo "Results" > results_sc_1_${graphs[i]}_MQBucket.log
        ./SetCover_MQ -rounds 10 -delta 0 -threads 1 -queues 4 -buckets 64 -stick 1 -batch1 256 -batch2 256 -type MQBucket -prefetch ${input_dir}/${graphs[i]}.ag  >> results_sc_1_${graphs[i]}_MQBucket.log
done


# MQ
for i in {0..3}
do
        echo "Results" > results_sc_48_${graphs[i]}_MQ.log
        ./SetCover_MQ -rounds 10 -delta 0 -threads 48 -queues 192 -buckets 64 -stick ${stickinessheap[i]} -batch1 ${batches1[i]} -batch2 ${batches2[i]} -type MQ -prefetch ${input_dir}/${graphs[i]}.ag  >> results_sc_48_${graphs[i]}_MQ.log
done

for i in {0..3}
do
        echo "Results" > results_sc_1_${graphs[i]}_MQ.log
        ./SetCover_MQ -rounds 10 -delta 0 -threads 1 -queues 4 -buckets 64 -stick 1 -batch1 ${batches1[i]} -batch2 ${batches2[i]} -type MQ -prefetch ${input_dir}/${graphs[i]}.ag  >> results_sc_1_${graphs[i]}_MQ.log
done

# MQPlain
for i in {0..3}
do
    echo "Results" > results_sc_48_${graphs[i]}_MQPlain.log
    ./SetCover_MQ -rounds 10 -threads 48 -queues 192 -batch1 1 -batch2 1 -type MQ ${input_dir}/${graphs[i]}.ag  >> results_sc_48_${graphs[i]}_MQPlain.log
done

for i in {0..3}
do
    echo "Results" > results_sc_1_${graphs[i]}_MQPlain.log
    ./SetCover_MQ -rounds 10 -threads 1 -queues 4 -batch1 1 -batch2 1 -type MQ ${input_dir}/${graphs[i]}.ag  >> results_sc_1_${graphs[i]}_MQPlain.log
done

# ./SetCover_MQ -type SkiphashPQ -rounds 1 -threads 1 -delta 0 -strict 1 -batch 0 -chunksize 128 -s inputs/roadnetCA.adj

# ./SetCover_MQ -type SkiphashPQ -rounds 1 -threads 96 -delta 0 -strict 0 -batch 1 -chunksize 128 -s inputs/roadnetCA.adj

# ./SetCover_MQ -rounds 1 -threads 96 -queues 192 -batch1 128 -batch2 128 -type MQBucket -s inputs/roadnetCA.adj



t=96
LD_PRELOAD=/usr/lib/x86_64-linux-gnu/libjemalloc.so.2 ./SetCover_MQ -type SkiphashPQ -rounds 1 -threads $t -delta 0 -strict 1 -batch 0 -chunksize 128 -numchunks 128 -s inputs/roadnetCA.adj


t=96
q=$((t * 2))
b=128
LD_PRELOAD=/usr/lib/x86_64-linux-gnu/libjemalloc.so.2 ./SetCover_MQ -rounds 1 -threads $t -queues $q -batch1 $b -batch2 $b -type MQBucket -s inputs/orkut.adj



t=96
./SetCover_MQ -type SkiphashPQ -rounds 1 -threads $t -delta 0 -strict 0 -batch 1 -chunksize 8 -numchunks 64 -s inputs/orkut.adj


t=96
./SetCover_MQ -type Linden -rounds 1 -threads $t -delta 0 -strict 0 -batch 1 -chunksize 8 -numchunks 64 -s inputs/orkut.adj

