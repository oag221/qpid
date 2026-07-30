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
algs=(bfs sssp ppsp)
graphs=(orkut livejournal roadnetCA) # livejournal
dest_node=(3000000 3000000 300000) # corresponds to each graph
startnode=1
threads=(1 12 24 48 96)
delta=(0 1 2)

trials=3

# Output configs
cols_txt="%8s %12s %7s %12s %7s %12s %10s %12s %9s %10s %13s"
cols_csv="%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s"
headers="step ds alg exp delta graph n_queues chunk_size threads time(ms) wasted_work"

printf "${cols_txt}\n" ${headers} > $summary_txt
printf "${cols_csv}\n" ${headers} > $summary_csv
cat $summary_txt

cnt1=10000

#########################################################
#########################################################
#########################################################

# #! SkipHashPQ
ds="SkipHashPQ"

# SkipHashPQ specific configs
chunksize=(32 64 128)
num_lanes=(64 128)
chunksize_roadnetCA=(4 8 16)
num_lanes_roadnetCA=(4 8 16)
exp_type=("strict" "relaxed" "batch") # "ord-batch"

printf "NOTE: for SkipHashPQ, \`n_queues\` for 1 thread is acually always 1 (printed as not 1 for convenience of grouping)\n\n"

for a in "${algs[@]}"; do
        for e in "${exp_type[@]}"; do
                batch_opt=0
                strict_opt=0
                ordered_opt=0
                # NOTE: if e == "relaxed", all remain 0
                if [[ "$e" == "strict" ]]; then
                        strict_opt=1
                elif [[ "$e" == "batch" ]]; then
                        batch_opt=1
                elif [[ "$e" == "ordered-batch" ]]; then
                        ordered_opt=1
                fi

                for d in "${delta[@]}"; do
                        if [[ "$a" == "PageRank" && $d -eq 0 ]]; then
                                d_val=28
                        elif [[ "$a" == "PageRank" ]]; then
                                continue
                        fi

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
                                if [ ! -d_val "$log_dir_a" ]; then
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

                                                        if [[ $a == "ppsp"|| $a == "sssp" ]]; then
                                                                g_name="w_1-10_${g}.gr"
                                                                if [[ $a == "ppsp" ]]; then
                                                                        # ppsp
                                                                        cmd="LD_PRELOAD=/usr/lib/x86_64-linux-gnu/libjemalloc.so.2 ${exec_dir}/${a}/${a}-cpu -startNode ${startnode} -destNode ${dest_node[$g_cntr]} -threads $t -delta $d_val -batch $batch_opt -strict $strict_opt -order_batch $ordered_opt -chunksize $c -num_chunks $n_queues -algo=${ds} ${w_input_dir}/${g_name}"
                                                                else
                                                                        # sssp
                                                                        cmd="LD_PRELOAD=/usr/lib/x86_64-linux-gnu/libjemalloc.so.2 ${exec_dir}/${a}/${a}-cpu -startNode ${startnode} -threads $t -delta $d_val -batch $batch_opt -strict $strict_opt -order_batch $ordered_opt -chunksize $c -num_chunks $n_queues -algo=${ds} ${w_input_dir}/${g_name}"
                                                                fi
                                                        elif [[ $a == "bfs" ]]; then
                                                                # bfs
                                                                cmd="LD_PRELOAD=/usr/lib/x86_64-linux-gnu/libjemalloc.so.2 ${exec_dir}/${a}/${a}-cpu -startNode ${startnode} -threads $t -delta $d_val -batch $batch_opt -strict $strict_opt -order_batch $ordered_opt -chunksize $c -num_chunks $n_queues -algo=${ds} ${input_dir}/${g}.gr"
                                                        else
                                                                # pagerank
                                                                cmd="LD_PRELOAD=/usr/lib/x86_64-linux-gnu/libjemalloc.so.2 ${exec_dir}/${a}/${a}-cpu -threads $t -delta $d_val -batch $batch_opt -strict $strict_opt -order_batch $ordered_opt -chunksize $c -num_chunks $n_queues -algo=${ds} ${input_dir}/${g}.gr"
                                                        fi

                                                        tot_time=0
                                                        tot_empty_work=0

                                                        for ((i = 1; i <= trials; i++)); do
                                                                filename="${log_dir_a}/${cnt1}_${ds}_results_${a}_g-${g}.c${c}_queues${n_queues}_threads${t}_trial${i}.log"
                                                                echo "Results" > $filename

                                                                eval $cmd >> $filename 2>&1
                                                                # ./build/lonestar/analytics/cpu/bfs/bfs-cpu -startNode 1 -delta 0 -algo=SkipHashPQ -threads 96 -num_chunks 16 -chunksize 8 inputs/livejournal.gr
                                                                ms=$(cat $filename | grep -oP 'runtime_ms \K[0-9]+')
                                                                (( tot_time += ms ))
                                                                empty_work=$(cat $filename | grep -oP 'totalEmptyWork \K[0-9]+')
                                                                (( tot_empty_work += empty_work ))
                                                        done

                                                        avg_time=$((tot_time / trials))
                                                        avg_empty_work=$((tot_empty_work / trials))
                                                        
                                                        printf "${cols_txt}\n" $cnt1 $ds $a $e $d_val $g $q $c $t $avg_time $avg_empty_work >> $summary_txt
                                                        printf "${cols_csv}\n" $cnt1 $ds $a $e $d_val $g $q $c $t $avg_time $avg_empty_work >> $summary_csv
                                                        tail -1 $summary_txt
                                                        
                                                        cnt1=`expr $cnt1 + 1`
                                                done
                                        done
                                done
                                g_cntr=`expr $g_cntr + 1`
                        done
                done
        done
done

#########################################################
#########################################################
#########################################################

#! MQBucket
ds="MQBucket"

echo
echo "Running MQBucket with: (prefetch)=0,(buckets)=64" >> $summary_txt

# MQBucket output
headers_mqbucket="step ds alg batch_size delta graph n_queues stick threads time(ms) wasted_work"
printf "\n${cols_txt}\n" ${headers_mqbucket} >> $summary_txt
printf "${cols_csv}\n" ${headers_mqbucket} >> $summary_csv
tail -3 $summary_txt

prefetch=0
stickiness=(1 4 16)
batch_size=(1 64 128)
for a in "${algs[@]}"; do
        for b in "${batch_size[@]}"; do
                for d in "${delta[@]}"; do
                        if [[ "$a" == "PageRank" && $d -eq 0 ]]; then
                                d_val=28
                        elif [[ "$a" == "PageRank" ]]; then
                                continue
                        fi

                        g_cntr=0
                        for g in "${graphs[@]}"; do
                                for s in "${stickiness[@]}"; do
                                        log_dir_a=$log_dir/$a
                                        if [ ! -d "$log_dir_a" ]; then
                                                mkdir "$log_dir_a"
                                        fi

                                        for t in "${threads[@]}"; do
                                                num_queues=$((t * 2))

                                                if [[ $a == "ppsp"|| $a == "sssp" ]]; then
                                                        g_name="w_1-10_${g}.gr"
                                                        if [[ $a == "ppsp" ]]; then
                                                                # ppsp
                                                                cmd="LD_PRELOAD=/usr/lib/x86_64-linux-gnu/libjemalloc.so.2 ${exec_dir}/${a}/${a}-cpu -startNode ${startnode} -destNode ${dest_node[$g_cntr]} -delta $d_val -prefetch ${prefetch} -batch1 $b -batch2 $b -stick $s -buckets 64 -threads $t -queues ${num_queues} -algo=${ds} ${w_input_dir}/${g_name}"
                                                        else
                                                                # sssp
                                                                cmd="LD_PRELOAD=/usr/lib/x86_64-linux-gnu/libjemalloc.so.2 ${exec_dir}/${a}/${a}-cpu -startNode ${startnode} -delta $d_val -prefetch ${prefetch} -batch1 $b -batch2 $b -stick $s -buckets 64 -threads $t -queues ${num_queues} -algo=${ds} ${w_input_dir}/${g_name}"
                                                        fi 
                                                else
                                                        # bfs
                                                        cmd="LD_PRELOAD=/usr/lib/x86_64-linux-gnu/libjemalloc.so.2 ${exec_dir}/${a}/${a}-cpu -startNode ${startnode} -threads $t -algo=${ds} ${input_dir}/${g}.gr"
                                                fi

                                                tot_time=0
                                                tot_empty_work=0

                                                for ((i = 1; i <= trials; i++)); do
                                                        filename="${log_dir_a}/${cnt1}_${ds}_results_${a}_g-${g}.stick${s}_queues${num_queues}_threads${t}_trial${i}.log"
                                                        echo "Results" > $filename
                                                        eval $cmd >> $filename 2>&1

                                                        ms=$(cat $filename | grep -oP 'runtime_ms \K[0-9]+')
                                                        (( tot_time += ms ))
                                                        empty_work=$(cat $filename | grep -oP 'totalEmptyWork \K[0-9]+')
                                                        (( tot_empty_work += empty_work ))
                                                done

                                                avg_time=$((tot_time / 3))
                                                avg_empty_work=$((tot_empty_work / 3))
                                                
                                                printf "${cols_txt}\n" $cnt1 $ds $a $b $d_val $g $num_queues $s $t $avg_time $avg_empty_work >> $summary_txt
                                                printf "${cols_csv}\n" $cnt1 $ds $a $b $d_val $g $num_queues $s $t $avg_time $avg_empty_work >> $summary_csv
                                                tail -1 $summary_txt

                                                cnt1=`expr $cnt1 + 1`
                                        done
                                done
                                g_cntr=`expr $g_cntr + 1`
                        done
                done
        done
done

#########################################################
#########################################################
#########################################################

#! PIPQ, SMQ, Linden, Spray
ds=("PIPQ" "SMQ" "Linden" "Spray")

preload="LD_PRELOAD=/usr/lib/x86_64-linux-gnu/libjemalloc.so.2 "

for d in "${ds[@]}"; do
        for a in "${algs[@]}"; do
                g_cntr=0
                for g in "${graphs[@]}"; do
                        log_dir_a=$log_dir/$a
                        if [ ! -d "$log_dir_a" ]; then
                                mkdir "$log_dir_a"
                        fi

                        for t in "${threads[@]}"; do
                                if [[ $d == "Linden" ]]; then
                                        cmd=""
                                else
                                        cmd=$preload
                                fi

                                if [[ $a == "ppsp"|| $a == "sssp" ]]; then
                                        g_name="w_1-10_${g}.gr"
                                        if [[ $a == "ppsp" ]]; then
                                                # ppsp
                                                cmd+="${exec_dir}/${a}/${a}-cpu -startNode ${startnode} -destNode ${dest_node[$g_cntr]} -threads $t -algo=${d} ${w_input_dir}/${g_name}"
                                        else
                                                # sssp
                                                cmd+="${exec_dir}/${a}/${a}-cpu -startNode ${startnode} -threads $t -algo=${d} ${w_input_dir}/${g_name}"
                                        fi 
                                else
                                        # bfs
                                        cmd+="${exec_dir}/${a}/${a}-cpu -startNode ${startnode} -threads $t -algo=${d} ${input_dir}/${g}.gr"
                                fi

                                time=0
                                empty_work=0

                                for ((i = 1; i <= trials; i++)); do
                                        filename="${log_dir_a}/${cnt1}_${d}_results_${a}_g-${g}.stick${s}_queues${num_queues}_threads${t}_trial${i}.log"
                                        echo "Results" > $filename
                                        eval $cmd >> $filename 2>&1

                                        cur_time=$(cat $filename | grep -oP 'runtime_ms \K[0-9]+')
                                        cur_empty_work=$(cat $filename | grep -oP 'totalEmptyWork \K[0-9]+')

                                        ((time += cur_time))
                                        ((empty_work += cur_empty_work))
                                done

                                avg_time=$((time / 3))
                                avg_empty_work=$((empty_work / 3))

                                printf "${cols_txt}\n" $cnt1 $d $a x x $g x x $t $avg_time $avg_empty_work >> $summary_txt
                                printf "${cols_csv}\n" $cnt1 $d $a x x $g x x $t $avg_time $avg_empty_work >> $summary_csv
                                tail -1 $summary_txt

                                cnt1=`expr $cnt1 + 1`
                        done
                        g_cntr=`expr $g_cntr + 1`
                done
        done
done









# LD_PRELOAD=/usr/lib/x86_64-linux-gnu/libjemalloc.so.2 ./build/lonestar/analytics/cpu/bfs/bfs-cpu -startNode 1 -delta 0 -prefetch 1 -stick 1 -chunksize 128 -num_chunks 64 -threads 1 -algo=SkipHashPQ inputs/livejournal.gr

###########################
### MQBucket
###########################

# threads=96
# queues=$((threads * 2))
# LD_PRELOAD=/usr/lib/x86_64-linux-gnu/libjemalloc.so.2 ./build/lonestar/analytics/cpu/sssp/sssp-cpu -startNode 1 -delta 0 -prefetch 0 -stick 8 -batch1 1024 -batch2 1024 -buckets 64 -threads $threads -queues $queues -algo=MQBucket inputs/weighted_inputs/w_1-100_orkut.gr


delta=22
LD_PRELOAD=/usr/lib/x86_64-linux-gnu/libjemalloc.so.2 ./build/lonestar/analytics/cpu/pagerank/pagerank-push-cpu -delta ${delta} -prefetch 0 -stick 8 -batch1 128 -batch2 128 -buckets 64 -threads 96 -queues 192 -algo=MQBucket inputs/roadnetCA.gr



### PPSP
# LD_PRELOAD=/usr/lib/x86_64-linux-gnu/libjemalloc.so.2 ./build/lonestar/analytics/cpu/ppsp/ppsp-cpu -startNode 1 -destNode 3000000 -delta 0 -prefetch 0 -stick 4 -batch1 128 -batch2 128 -buckets 64 -threads 96 -queues 192 -algo=MQBucket inputs/weighted_inputs/w_1-10_

###########################
### SkipHashPQ
###########################

### PPSP
# LD_PRELOAD=/usr/lib/x86_64-linux-gnu/libjemalloc.so.2 ./build/lonestar/analytics/cpu/sssp/sssp-cpu -startNode 1 -chunksize 8 -num_chunks 8 -threads 96 -batch 0 -strict 1 -delta 0 -algo=SkipHashPQ inputs/weighted_inputs/w_1-10_roadnetCA.gr

### BFS
# LD_PRELOAD=/usr/lib/x86_64-linux-gnu/libjemalloc.so.2 ./build/lonestar/analytics/cpu/sssp/sssp-cpu -startNode 1 -algo=SkipHashPQ -threads 96 -num_chunks 128 -chunksize 64 -batch 1 -delta 0 -strict 0 inputs/weighted_inputs/w_1-100_orkut.gr


delta=18
LD_PRELOAD=/usr/lib/x86_64-linux-gnu/libjemalloc.so.2 ./build/lonestar/analytics/cpu/pagerank/pagerank-push-cpu -algo=SkipHashPQ -num_chunks 128 -chunksize 128 -threads 96 -batch 1 -order_batch 0 -delta ${delta} -strict 0 inputs/orkut.gr

# threads=96
# LD_PRELOAD=/usr/lib/x86_64-linux-gnu/libjemalloc.so.2 ./build/lonestar/analytics/cpu/bfs/bfs-cpu -startNode 1 -algo=SkipHashPQ -num_chunks 64 -chunksize 64 -threads $threads -batch 1 -order_batch 0 -delta 0 -strict 0 inputs/livejournal.gr

### BFS - batch-1
# roadnet: LD_PRELOAD=/usr/lib/x86_64-linux-gnu/libjemalloc.so.2 ./build/lonestar/analytics/cpu/bfs/bfs-cpu -startNode 1 -algo=SkipHashPQ -num_chunks 16 -chunksize 16 -threads 96 -batch 1 -delta 0 inputs/roadnetCA.gr
# others: LD_PRELOAD=/usr/lib/x86_64-linux-gnu/libjemalloc.so.2 ./build/lonestar/analytics/cpu/bfs/bfs-cpu -startNode 1 -algo=SkipHashPQ -num_chunks 64 -chunksize 64 -threads 96 -batch 0 -delta 0 -strict 1 inputs/orkut.gr

### BFS - batch-2
# LD_PRELOAD=/usr/lib/x86_64-linux-gnu/libjemalloc.so.2 ./build/lonestar/analytics/cpu/bfs/bfs-cpu -startNode 1 -algo=SkipHashPQ -num_chunks 128 -chunksize 64 -threads 96 -batch 2 inputs/orkut.gr


###########################
### PIPQ
###########################

# threads=1
# LD_PRELOAD=/usr/lib/x86_64-linux-gnu/libjemalloc.so.2 ./build/lonestar/analytics/cpu/ppsp/ppsp-cpu -startNode 1 -threads $threads -algo=PIPQ inputs/weighted_inputs/w_1-10_livejournal.gr

###########################
### Linden
###########################
threads=96
./build/lonestar/analytics/cpu/pagerank/pagerank-push-cpu -threads $threads -algo=Linden inputs/weighted_inputs/w_1-10_roadnetCA.gr


LD_PRELOAD=/usr/lib/x86_64-linux-gnu/libjemalloc.so.2 ./build/lonestar/analytics/cpu/pagerank/pagerank-push-cpu -threads 96 -chunksize 32 -num_chunks 64 -batch 1 -strict 0 -delta 28 -algo=SkipHashPQ inputs/orkut.gr

###########################
### Spraylist
###########################

# for 
# threads=12
# ./build/lonestar/analytics/cpu/ppsp/ppsp-cpu -startNode 1 -destNode 3000000 -threads $threads -algo=Spray inputs/weighted_inputs/w_1-10_livejournal.gr

###########################
### SMQ
###########################

# threads=96
# LD_PRELOAD=/usr/lib/x86_64-linux-gnu/libjemalloc.so.2 ./build/lonestar/analytics/cpu/ppsp/ppsp-cpu -startNode 1 -destNode 300000 -steal_prob 16 -steal_size 128 -threads $threads -algo=SMQ inputs/weighted_inputs/w_1-10_roadnetCA.gr
