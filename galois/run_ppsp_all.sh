#!/bin/bash

#########################################################
#########################################################
#### Setup directories & output/log files 
#########################################################
#########################################################

alg=ppsp

w_input_dir=inputs/weighted_inputs

output_dir=outputs_${alg}
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

# For output configs
cols_txt="%8s %12s %7s %12s %7s %18s %10s %12s %9s %10s %13s"
cols_csv="%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s"

cnt1=10000

#########################################################
#########################################################
#### Alg & dataset configs
#########################################################
#########################################################

threads=(1 12 24 48 96)
delta=0
trials=3
exec_dir=./build/lonestar/analytics/cpu
#datasets=(orkut livejournal roadnetCA)
# datasets=(twitter-2010 soc-LiveJournal1 orkut roadnetCA)
datasets=(soc-LiveJournal1)
dest_node=(3000000 3000000 3000000 300000) # corresponds to each dataset
startNode=1

#########################################################
#########################################################
#### Run tests
#########################################################
#########################################################

for cur_ds in "$@"; do
        if [[ $cur_ds == "ALL" || $cur_ds == "SkipHashPQ" ]]; then
                #########################################################
                #########################################################
                #### SkipHashPQ
                #########################################################
                #########################################################
                ds="SkipHashPQ"
                printf "NOTE: for SkipHashPQ, \`n_queues\` for 1 thread is acually always 1 (printed as not 1 for convenience of grouping)\n\n"

                headers="step ds alg exp delta graph n_queues chunk_size threads time(ms) wasted_work"
                printf "${cols_txt}\n" ${headers} >> $summary_txt
                printf "${cols_csv}\n" ${headers} >> $summary_csv
                tail $summary_txt
                
                exp_type=("strict" "batch")

                for e in "${exp_type[@]}"; do
                        batch_opt=0
                        strict_opt=0
                        if [[ "$e" == "strict" ]]; then
                                strict_opt=1
                        elif [[ "$e" == "batch" ]]; then
                                batch_opt=1
                        fi

                        g_cntr=0
                        for g in "${datasets[@]}"; do
                                g_name="w_1-10_${g}.gr"

                                if [[ "$g" == "roadnetCA" ]]; then
                                        chunksize=(4 16)
                                        n_queues=16
                                else
                                        chunksize=(64)
                                        n_queues=128
                                fi

                                for c in "${chunksize[@]}"; do
                                        for t in "${threads[@]}"; do
                                                if [ "$t" -eq 1 ]; then
                                                        q=1
                                                else
                                                        q=$n_queues
                                                fi
                                                
                                                cmd="LD_PRELOAD=/usr/lib/x86_64-linux-gnu/libjemalloc.so.2 ${exec_dir}/${alg}/${alg}-cpu -startNode ${startNode} -destNode ${dest_node[$g_cntr]} -threads $t -delta 0 -batch $batch_opt -strict $strict_opt -chunksize $c -num_chunks $q -algo=${ds} ${w_input_dir}/${g_name}"

                                                tot_time=0
                                                tot_empty_work=0
                                                for ((i = 1; i <= trials; i++)); do
                                                        filename="${log_dir}/${cnt1}_${ds}-${e}_results_${alg}_${g}_chunksize${c}_queues${n_queues}_delta${delta}_threads${t}_trial${i}.log"
                                                        echo "${cmd}" >> $filename
                                                        
                                                        echo "Results" >> $filename
                                                        eval $cmd >> $filename 2>&1

                                                        ms=$(cat $filename | grep -oP 'runtime_ms \K[0-9]+')
                                                        empty_work=$(cat $filename | grep -oP 'totalEmptyWork \K[0-9]+')

                                                        (( tot_time += ms))
                                                        (( tot_empty_work += empty_work))
                                                done

                                                avg_time=$((tot_time / trials))
                                                avg_empty_work=$((tot_empty_work / trials))
                                                
                                                printf "${cols_txt}\n" $cnt1 $ds $alg $e ${delta} $g $n_queues $c $t $avg_time $avg_empty_work >> $summary_txt
                                                printf "${cols_csv}\n" $cnt1 $ds $alg $e ${delta} $g $n_queues $c $t $avg_time $avg_empty_work >> $summary_csv
                                                tail -1 $summary_txt
                                                
                                                cnt1=`expr $cnt1 + 1`
                                        done
                                done
                                g_cntr=`expr $g_cntr + 1`
                        done
                done
        fi
        if [[ $cur_ds == "ALL" || $cur_ds == "MQBucket" ]]; then
                #########################################################
                #########################################################
                #### MQBucket
                #########################################################
                #########################################################
                ds="MQBucket"

                headers_mqbucket="step ds alg batch_size delta graph n_queues stick threads time(ms) wasted_work"
                printf "\n${cols_txt}\n" ${headers_mqbucket} >> $summary_txt
                printf "${cols_csv}\n" ${headers_mqbucket} >> $summary_csv
                tail -1 $summary_txt

                prefetch=0
                stickiness=(8)
                batch_size=(1 8 32 128)

                g_cntr=0
                for g in "${datasets[@]}"; do
                        g_name="w_1-10_${g}.gr"
                        for b in "${batch_size[@]}"; do
                                for s in "${stickiness[@]}"; do
                                        for t in "${threads[@]}"; do
                                                num_queues=$((t * 2))
                                                cmd="LD_PRELOAD=/usr/lib/x86_64-linux-gnu/libjemalloc.so.2 ${exec_dir}/${alg}/${alg}-cpu -startNode ${startNode} -destNode ${dest_node[$g_cntr]} -threads $t -queues ${num_queues} -delta 0 -batch1 ${b} -batch2 ${b} -stick $s -buckets 64 -algo=${ds} ${w_input_dir}/${g_name}"
                                                
                                                tot_time=0
                                                tot_empty_work=0
                                                for ((i = 1; i <= trials; i++)); do
                                                        filename="${log_dir}/${cnt1}_${ds}_results_${ds}_${g}_batch${b}_delta${delta}_stick${s}_threads${t}_trial${i}.log"
                                                        echo "Results" > $filename
                                                        echo "${cmd}" >> $filename
                                                        eval $cmd >> $filename 2>&1

                                                        ms=$(cat $filename | grep -oP 'runtime_ms \K[0-9]+')
                                                        empty_work=$(cat $filename | grep -oP 'totalEmptyWork \K[0-9]+')
                                                        
                                                        (( tot_time += ms))
                                                        (( tot_empty_work += empty_work))
                                                done

                                                avg_time=$((tot_time / trials))
                                                avg_empty_work=$((tot_empty_work / trials))
                                                
                                                printf "${cols_txt}\n" $cnt1 $ds $alg $b $delta $g $num_queues $s $t $avg_time $avg_empty_work >> $summary_txt
                                                printf "${cols_csv}\n" $cnt1 $ds $alg $b $delta $g $num_queues $s $t $avg_time $avg_empty_work >> $summary_csv
                                                tail -1 $summary_txt
                                                
                                                cnt1=`expr $cnt1 + 1`
                                        done
                                done
                        done
                        g_cntr=`expr $g_cntr + 1`
                done
        fi
        if [[ $cur_ds == "ALL" || $cur_ds == "PIPQ"  || $cur_ds == "Linden" || $cur_ds == "Spraylist" || $cur_ds == "SMQ" ]]; then
                #########################################################
                #########################################################
                #### PIPQ, SMQ, Linden, Spray
                #########################################################
                #########################################################
                if [[ $cur_ds == "ALL" ]]; then
                        pqs=("Linden" "SMQ" "PIPQ" "Spray")
                else
                        pqs=($cur_ds)
                fi

                headers_compet="step ds alg exp delta graph n_queues chunk_size threads time(ms) wasted_work"
                printf "${cols_txt}\n" ${headers_compet} >> $summary_txt
                printf "${cols_csv}\n" ${headers_compet} >> $summary_csv
                tail $summary_txt

                for ds in "${pqs[@]}"; do
                        g_cntr=0
                        for g in "${datasets[@]}"; do
                                g_name="w_1-10_${g}.gr"
                                for t in "${threads[@]}"; do
                                        if [[ $ds == "Linden" ]]; then
                                                cmd="./build/lonestar/analytics/cpu/${alg}/${alg}-cpu -startNode ${startNode} -destNode ${dest_node[$g_cntr]} -threads $t -algo=${ds} ${w_input_dir}/${g_name}"
                                        else
                                                cmd="LD_PRELOAD=/usr/lib/x86_64-linux-gnu/libjemalloc.so.2 ./build/lonestar/analytics/cpu/${alg}/${alg}-cpu -startNode ${startNode} -destNode ${dest_node[$g_cntr]} -threads $t -algo=${ds} ${w_input_dir}/${g_name}"
                                        fi

                                        tot_time=0
                                        tot_empty_work=0

                                        for ((i = 1; i <= trials; i++)); do
                                                filename="${log_dir}/${cnt1}_${ds}_results_${alg}_${g}_threads${t}_trial${i}.log"
                                                echo "Results" > $filename
                                                eval $cmd >> $filename 2>&1

                                                ms=$(cat $filename | grep -oP 'runtime_ms \K[0-9]+')
                                                empty_work=$(cat $filename | grep -oP 'totalEmptyWork \K[0-9]+')

                                                (( tot_time += ms))
                                                (( tot_empty_work += empty_work))
                                        done

                                        avg_time=$((tot_time / trials))
                                        avg_empty_work=$((tot_empty_work / trials))
                                        
                                        printf "${cols_txt}\n" $cnt1 $ds $alg 0 0 $g 0 0 $t $avg_time $avg_empty_work >> $summary_txt
                                        printf "${cols_csv}\n" $cnt1 $ds $alg 0 0 $g 0 0 $t $avg_time $avg_empty_work >> $summary_csv
                                        tail -1 $summary_txt
                                        
                                        cnt1=`expr $cnt1 + 1`
                                done
                                g_cntr=`expr $g_cntr + 1`
                        done
                done
        fi
done
