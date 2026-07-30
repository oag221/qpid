#!/bin/bash

# Setup directories & files 
input_dir=inputs
w_input_dir=inputs/weighted_inputs

output_dir=outputs
log_dir=$output_dir/logs
summary_txt=$output_dir/summary.txt
summary_csv=$output_dir/summary.csv

if [ -d $output_dir ]; then
        if [ -d $output_dir.old ]; then
                rm -rf $output_dir.old
        fi
        mv -f $output_dir $output_dir.old
fi

mkdir $output_dir
mkdir $log_dir
touch $summary_txt
touch $summary_csv

exec_dir=./build/lonestar/analytics/cpu

# Alg & Graph configs
algs=(ppsp sssp bfs)
graphs=(orkut livejournal roadnetCA) # livejournal
dest_node=(3000000 3000000 300000) # corresponds to each graph
startnode=1
threads=(1 12 24 48 96)

# SkipHashPQ specific configs
chunksize=(32 64 128 256)
num_lanes=(32 64 128 256)
chunksize_roadnetCA=(1 4 8 16 32)
num_lanes_roadnetCA=(1 4 8 16 32)

# Output configs
cols_txt="%8s %12s %14s %14s %12s %12s %12s %12s %14s"
cols_csv="%s,%s,%s,%s,%s,%s,%s,%s,%s"
headers="step ds alg graph n_queues chunk_size threads time(ms) wasted_work"

printf "${cols_txt}\n" ${headers} > $summary_txt
printf "${cols_csv}\n" ${headers} > $summary_csv
cat $summary_txt

cnt1=10000

#! SkipHashPQ
ds="SkipHashPQ"
for a in "${algs[@]}"; do
        g_cntr=0
        for g in "${graphs[@]}"; do
                if [[ "$g" == "roadnetCA" ]]; then
                        chunksize_=("${chunksize_roadnetCA[@]}")
                        num_lanes_=("${num_lanes_roadnetCA[@]}")
                else
                        chunksize_=("${chunksize[@]}")
                        num_lanes_=("${num_lanes[@]}")
                fi

                log_dir_a=$log_dir/$a
                if [ ! -d "$log_dir_a" ]; then
                        mkdir "$log_dir_a"
                fi

                for q in "${num_lanes_[@]}"; do
                        for c in "${chunksize_[@]}"; do
                                for t in "${threads[@]}"; do
                                        if [ "$t" -eq 1 ]; then
                                                n_queues=1
                                        else
                                                n_queues=$q 
                                        fi

                                        filename="${log_dir_a}/$cnt1.results_${a}_g-${g}.c${c}_queues${n_queues}_threads${t}_${ds}.log"
                                        echo "Results" > $filename

                                        if [[ $a == "ppsp"|| $a == "sssp" ]]; then
                                                g_name="w_1-10_${g}.gr"
                                                if [[ $a == "ppsp" ]]; then
                                                        # ppsp
                                                        cmd="LD_PRELOAD=/usr/lib/x86_64-linux-gnu/libjemalloc.so.2 ${exec_dir}/${a}/${a}-cpu -startNode ${startnode} -destNode ${dest_node[$g_cntr]} -threads $t -prefetch 1 -stick 1 -chunksize $c -num_chunks $n_queues -algo=${ds} ${w_input_dir}/${g_name}"
                                                else
                                                        # sssp
                                                        cmd="LD_PRELOAD=/usr/lib/x86_64-linux-gnu/libjemalloc.so.2 ${exec_dir}/${a}/${a}-cpu -startNode ${startnode} -threads $t -prefetch 1 -stick 1 -chunksize $c -num_chunks $n_queues -algo=${ds} ${w_input_dir}/${g_name}"
                                                fi
                                        else
                                                # bfs
                                                cmd="LD_PRELOAD=/usr/lib/x86_64-linux-gnu/libjemalloc.so.2 ${exec_dir}/${a}/${a}-cpu -startNode ${startnode} -threads $t -prefetch 1 -stick 1 -chunksize $c -num_chunks $n_queues -algo=${ds} ${input_dir}/${g}.gr"
                                        fi

                                        eval $cmd >> $filename 2>&1
                                        # ./build/lonestar/analytics/cpu/bfs/bfs-cpu -startNode 1 -delta 0 -algo=SkipHashPQ -threads 96 -num_chunks 16 -chunksize 8 inputs/livejournal.gr
                                        time=$(cat $filename | grep -oP 'runtime_ms \K[0-9]+')
                                        empty_work=$(cat $filename | grep -oP 'totalEmptyWork \K[0-9]+')

                                        printf "${cols_txt}\n" $cnt1 $ds $a $g $n_queues $c $t $time $empty_work >> $summary_txt
                                        printf "${cols_csv}\n" $cnt1 $ds $a $g $n_queues $c $t $time $empty_work >> $summary_csv
                                        tail -1 $summary_txt
                                        
                                        cnt1=`expr $cnt1 + 1`
                                done
                        done
                done
                g_cntr=`expr $g_cntr + 1`
        done
done

# MQBucket output
headers_mqbucket="step ds alg graph n_queues stick threads time(ms) wasted_work"

printf "${cols_txt}\n" ${headers_mqbucket} >> $summary_txt
printf "${cols_csv}\n" ${headers_mqbucket} >> $summary_csv
tail -1 $summary_txt

#! MQBucket - no batching
ds="MQBucket"
delta=0
prefetch=0
stickiness=(1 4 16 32)
echo "Running MQBucket with: NO batching, (delta,prefetch)=0, (stick)=1, (buckets)=64" >> $summary_txt
tail -1 $summary_txt
for a in "${algs[@]}"; do
        g_cntr=0
        for g in "${graphs[@]}"; do
                for s in "${stickiness[@]}"; do
                        log_dir_a=$log_dir/$a
                        if [ ! -d "$log_dir_a" ]; then
                                mkdir "$log_dir_a"
                        fi

                        for t in "${threads[@]}"; do
                                num_queues=$((t * 2))

                                filename="${log_dir_a}/$cnt1.results_${a}_g-${g}.stick${s}_queues${num_queues}_threads${t}_${ds}.log"
                                echo "Results" > $filename

                                if [[ $a == "ppsp"|| $a == "sssp" ]]; then
                                        g_name="w_1-10_${g}.gr"
                                        if [[ $a == "ppsp" ]]; then
                                                # ppsp
                                                cmd="LD_PRELOAD=/usr/lib/x86_64-linux-gnu/libjemalloc.so.2 ${exec_dir}/${a}/${a}-cpu -startNode ${startnode} -destNode ${dest_node[$g_cntr]} -delta ${delta} -prefetch ${prefetch} -batch1 1 -batch2 1 -stick $s -buckets 64 -threads $t -queues ${num_queues} -algo=${ds} ${w_input_dir}/${g_name}"
                                        else
                                                # sssp
                                                cmd="LD_PRELOAD=/usr/lib/x86_64-linux-gnu/libjemalloc.so.2 ${exec_dir}/${a}/${a}-cpu -startNode ${startnode} -delta ${delta} -prefetch ${prefetch} -batch1 1 -batch2 1 -stick $s -buckets 64 -threads $t -queues ${num_queues} -algo=${ds} ${w_input_dir}/${g_name}"
                                        fi 
                                else
                                        # bfs
                                        cmd="LD_PRELOAD=/usr/lib/x86_64-linux-gnu/libjemalloc.so.2 ${exec_dir}/${a}/${a}-cpu -startNode ${startnode} -delta ${delta} -prefetch ${prefetch} -batch1 1 -batch2 1 -stick $s -buckets 64 -threads $t -queues ${num_queues} -algo=${ds} ${input_dir}/${g}.gr"
                                fi

                                eval $cmd >> $filename 2>&1

                                time=$(cat $filename | grep -oP 'runtime_ms \K[0-9]+')
                                empty_work=$(cat $filename | grep -oP 'totalEmptyWork \K[0-9]+')

                                printf "${cols_txt}\n" $cnt1 $ds $a $g $num_queues $s $t $time $empty_work >> $summary_txt
                                printf "${cols_csv}\n" $cnt1 $ds $a $g $num_queues $s $t $time $empty_work >> $summary_csv
                                tail -1 $summary_txt

                                cnt1=`expr $cnt1 + 1`
                        done
                done
                g_cntr=`expr $g_cntr + 1`
        done
done

# LD_PRELOAD=/usr/lib/x86_64-linux-gnu/libjemalloc.so.2 ./build/lonestar/analytics/cpu/bfs/bfs-cpu -startNode 1 -delta 0 -prefetch 1 -stick 1 -chunksize 128 -num_chunks 64 -threads 1 -algo=SkipHashPQ inputs/livejournal.gr

# ./build/lonestar/analytics/cpu/sssp/sssp-cpu 
# -startNode 1 -delta 0 -prefetch 1 -stick 1 -chunksize 128 -num_chunks 64 -threads 1 -algo=SkipHashPQ inputs/livejournal.gr

# LD_PRELOAD=/usr/lib/x86_64-linux-gnu/libjemalloc.so.2 ./build/lonestar/analytics/cpu/sssp/sssp-cpu -startNode 1 -delta 0 -prefetch 1 -stick 1 -chunksize 128 -num_chunks 64 -threads 96 -algo=SkipHashPQ inputs/weighted_inputs/weighted_livejournal.gr

# LD_PRELOAD=/usr/lib/x86_64-linux-gnu/libjemalloc.so.2 ./build/lonestar/analytics/cpu/bfs/bfs-cpu -startNode 1 -delta 0 -prefetch 0 -stick 1 -batch1 1 -batch2 1 -buckets 64 -threads 96 -queues 192 -algo=MQBucket inputs/roadnetCA.gr

# LD_PRELOAD=/usr/lib/x86_64-linux-gnu/libjemalloc.so.2 ./build/lonestar/analytics/cpu/sssp/sssp-cpu -startNode 1 -batch 1 -algo=SkipHashPQ -num_chunks 32 -chunksize 64 -threads 96 inputs/weighted_inputs/w_1-10_orkut.gr

# LD_PRELOAD=/usr/lib/x86_64-linux-gnu/libjemalloc.so.2 ./build/lonestar/analytics/cpu/bfs/bfs-cpu -startNode 1 -chunksize 64 -num_chunks 64 -batch 2 -threads 96 -delta 1 -algo=SkipHashPQ inputs/orkut.gr