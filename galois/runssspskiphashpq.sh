#!/bin/bash

input_dir=inputs/weighted_inputs/
output_dir=sssp-data/outputs/
summary_txt=$output_dir/summary.txt
summary_csv=$output_dir/summary.csv

exec_dir=./build/lonestar/analytics/cpu/sssp

ds=SkipHashPQ

# graphs=(USAr LJ TW HYPERH)
graphs=(weighted_orkut weighted_roadnetCA weighted_livejournal)
chunksize=(1 4 8 16 32 128)
num_chunks=(1 2 4 8 16 24 32)
threads=(1 12 24 48 96)

startnode=1

cols_txt="%8s %12s %14s %12s %12s %12s %12s"
cols_csv="%s,%s,%s,%s,%s,%s,%s"
headers="step ds graph n_queues chunk_size threads time(ms)"

printf "${cols_txt}\n" ${headers} > $summary_txt
printf "${cols_csv}\n" ${headers} > $summary_csv
cat $summary_txt

cnt1=10000

# SkipHashPQ
for g in "${graphs[@]}"; do
        for q in "${num_chunks[@]}"; do
                for c in "${chunksize[@]}"; do
                        for t in "${threads[@]}"; do
                                filename="${output_dir}/$cnt1.results_sssp_g-${g}.c${c}_queues${q}_threads${t}_MQBucket.log"
                                echo "Results" > $filename

                                cmd="LD_PRELOAD=/usr/lib/x86_64-linux-gnu/libjemalloc.so.2 ${exec_dir}/sssp-cpu -startNode 1 -delta 0 -threads $t -prefetch 1 -stick 1 -chunksize $c -num_chunks $q -algo=SkipHashPQ ${input_dir}/${g}.gr"
                                
                                eval $cmd >> $filename 2>&1
                                # ./build/lonestar/analytics/cpu/sssp/sssp-cpu -startNode 1 -delta 0 -algo=SkipHashPQ -threads 96 -num_chunks 16 -chunksize 8 inputs/livejournal.gr
                                time=$(cat $filename | grep -oP 'runtime_ms \K[0-9]+')

                                printf "${cols_txt}\n" $cnt1 $ds $g $q $c $t $time >> $summary_txt
                                printf "${cols_csv}\n" $cnt1 $ds $g $q $c $t $time >> $summary_csv
                                tail -1 $summary_txt
                                
                                cnt1=`expr $cnt1 + 1`
                        done
                done
        done
done



# LD_PRELOAD=/usr/lib/x86_64-linux-gnu/libjemalloc.so.2 ./build/lonestar/analytics/cpu/sssp/sssp-cpu -startNode 1 -delta 0 -prefetch 1 -stick 1 -chunksize 128 -num_chunks 64 -threads 1 -algo=SkipHashPQ inputs/livejournal.gr


# ./build/lonestar/analytics/cpu/sssp/sssp-cpu 
# -startNode 1 -delta 0 -prefetch 1 -stick 1 -chunksize 128 -num_chunks 64 -threads 1 -algo=SkipHashPQ inputs/livejournal.gr


# LD_PRELOAD=/usr/lib/x86_64-linux-gnu/libjemalloc.so.2 ./build/lonestar/analytics/cpu/sssp/sssp-cpu -startNode 1 -delta 0 -prefetch 1 -stick 1 -chunksize 128 -num_chunks 64 -threads 96 -algo=SkipHashPQ inputs/weighted_inputs/weighted_livejournal.gr


# ./build/lonestar/analytics/cpu/sssp/sssp-cpu -startNode 1 -delta 0 -threads 48 -queues 96 -buckets 64 -batch1 256 -batch2 256 -prefetch 1 -stick 1 -algo=MQBucket  inputs/orkut.gr