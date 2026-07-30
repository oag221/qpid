#!/bin/bash

input_dir=../../inputs_galois
exec_dir=./build/lonestar/analytics/cpu/pagerank

graphs=(USA LJ TW HYPERH) 
bucket_deltas=(21 20 20 21)
mq_deltas=(20 15 19 18)
mq_plain_deltas=(20 15 15 15)

batch1=(256 128 256 64)
batch2=(256 128 256 64)

# MQBucket
for i in {0..3}
do
        echo "Results" > results_pr_48_${graphs[i]}_MQBucket.log
	for j in {1..10}
        do
		${exec_dir}/pagerank-push-cpu -threads 48 -queues 192 -delta ${bucket_deltas[i]} -buckets 64 -batch1 ${batch1[i]} -batch2 ${batch2[i]} -algo=MQBucket -prefetch 1 ${input_dir}/${graphs[i]}.gr >> results_pr_48_${graphs[i]}_MQBucket.log
        done
done

for i in {0..3}
do
        echo "Results" > results_pr_1_${graphs[i]}_MQBucket.log
        for j in {1..10}
        do
                ${exec_dir}/pagerank-push-cpu -threads 1 -queues 4 -delta ${bucket_deltas[i]} -buckets 64 -batch1 ${batch1[i]} -batch2 ${batch2[i]} -algo=MQBucket -prefetch 1 ${input_dir}/${graphs[i]}.gr >> results_pr_1_${graphs[i]}_MQBucket.log
        done
done


# MQ
for i in {0..3}
do
        echo "Results" > results_pr_48_${graphs[i]}_MQ.log
        for j in {1..10}
        do
                ${exec_dir}/pagerank-push-cpu -threads 48 -queues 192 -delta ${mq_deltas[i]} -buckets 64 -batch1 ${batch1[i]} -batch2 ${batch2[i]} -algo=MQ -prefetch 1 ${input_dir}/${graphs[i]}.gr >> results_pr_48_${graphs[i]}_MQ.log
        done
done

for i in {0..3}
do
        echo "Results" > results_pr_1_${graphs[i]}_MQ.log
        for j in {1..10}
        do
                ${exec_dir}/pagerank-push-cpu -threads 1 -queues 4 -delta ${mq_deltas[i]} -buckets 64 -batch1 ${batch1[i]} -batch2 ${batch2[i]} -algo=MQ -prefetch 1 ${input_dir}/${graphs[i]}.gr >> results_pr_1_${graphs[i]}_MQ.log
        done
done

# MQPlain
for i in {0..3}
do
        echo "Results" > results_pr_48_${graphs[i]}_MQPlain.log
        for j in {1..10}
        do
                ${exec_dir}/pagerank-push-cpu -threads 48 -queues 192 -delta ${mq_plain_deltas[i]} -buckets 64 -batch1 1 -batch2 1 -algo=MQ -prefetch 0 ${input_dir}/${graphs[i]}.gr >> results_pr_48_${graphs[i]}_MQPlain.log
        done
done

for i in {0..3}
do
        echo "Results" > results_pr_1_${graphs[i]}_MQPlain.log
        for j in {1..10}
        do
        	${exec_dir}/pagerank-push-cpu -threads 1 -queues 4 -delta ${mq_plain_deltas[i]} -buckets 64 -batch1 1 -batch2 1 -algo=MQ -prefetch 0 ${input_dir}/${graphs[i]}.gr >> results_pr_1_${graphs[i]}_MQPlain.log
	done
done

threads=96
queues=$((threads * 2))
LD_PRELOAD=/usr/lib/x86_64-linux-gnu/libjemalloc.so.2 ./build/lonestar/analytics/cpu/pagerank/pagerank-push-cpu -threads $threads -queues $queues -buckets 64 -delta 28 -batch1 1 -batch2 1 -stick 16 -algo=MQBucket inputs/livejournal.gr






threads=48
queues=$((threads * 2))
LD_PRELOAD=/usr/lib/x86_64-linux-gnu/libjemalloc.so.2 ./build/lonestar/analytics/cpu/pagerank/pagerank-push-cpu -threads $threads -queues $queues -buckets 64 -delta 28 -batch1 1 -batch2 1 -algo=MQ inputs/roadnetCA.gr


threads=96
q=128
c=32
LD_PRELOAD=/usr/lib/x86_64-linux-gnu/libjemalloc.so.2 ./build/lonestar/analytics/cpu/pagerank/pagerank-push-cpu -threads $threads -delta 28 -chunksize $c -num_chunks $q -strict 0 -batch 1 -order_batch 0 -algo=SkipHashPQ inputs/roadnetCA.gr


threads=96
./build/lonestar/analytics/cpu/pagerank/pagerank-push-cpu -threads $threads -algo=SMQ -delta 28 inputs/roadnetCA.gr


# ./build/lonestar/analytics/cpu/pagerank/pagerank-push-cpu -threads 96 -algo=SMQ inputs/roadnetCA.gr