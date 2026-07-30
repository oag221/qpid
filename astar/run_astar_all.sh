#!/bin/bash

#########################################################
#########################################################
#### Setup directories & output/log files 
#########################################################
#########################################################

input_dir=input
output_dir=outputs-astar
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
cols_txt="%8s %12s %7s %12s %7s %12s %10s %12s %9s %10s %13s"
cols_csv="%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s"

cnt1=10000

#########################################################
#########################################################
#### Alg & dataset configs
#########################################################
#########################################################

#threads=(1 12 24 48 96) #!
threads=(96)
delta=(1 2 4 6 8 10 11 12 14 15 16 18 20)
trials=3
exec=astar
dataset=germany.bin

#########################################################
#########################################################
#### Run tests
#########################################################
#########################################################

for cur_ds in "$@"; do
        if [[ $cur_ds == "ALL" || $cur_ds == "SkiphashPQ" ]]; then
                #########################################################
                #########################################################
                #### SkiphashPQ
                #########################################################
                #########################################################
                ds="SkiphashPQ"
                printf "NOTE: for SkipHashPQ, \`n_queues\` for 1 thread is acually always 1 (printed as not 1 for convenience of grouping)\n\n"

                headers="step ds alg exp delta graph n_queues chunk_size threads time(ms) wasted_work"
                printf "${cols_txt}\n" ${headers} >> $summary_txt
                printf "${cols_csv}\n" ${headers} >> $summary_csv
                tail $summary_txt

                # SkipHashPQ specific configs
                num_lanes=(8)
                chunksize=(4)
                # exp_type=("strict" "batch") #!
                exp_type=("strict")


                for e in "${exp_type[@]}"; do
                        batch_opt=0
                        strict_opt=0
                        if [[ "$e" == "strict" ]]; then
                                strict_opt=1
                        elif [[ "$e" == "batch" ]]; then
                                batch_opt=1
                        fi

                        for d in "${delta[@]}"; do
                                for q in "${num_lanes[@]}"; do
                                        for c in "${chunksize[@]}"; do
                                                for t in "${threads[@]}"; do
                                                        if [ "$t" -eq 1 ]; then
                                                                n_queues=1
                                                        else
                                                                n_queues=$q 
                                                        fi

                                                        cmd="LD_PRELOAD=/usr/lib/x86_64-linux-gnu/libjemalloc.so.2 ./build/astar input/germany.bin 0 320970 SkipHashPQ $t $c ${n_queues} ${strict_opt} ${batch_opt} $d"
                                                        # ./build/astar input/germany.bin 0 320970 SkipHashPQ 1 4 8 1 0 16"

                                                        tot_time=0
                                                        tot_empty_work=0
                                                        for ((i = 1; i <= trials; i++)); do
                                                                filename="${log_dir}/${cnt1}_${ds}-${e}_results_astar_germany.bin_chunksize${c}_queues${n_queues}_delta${d}_threads${t}_trial${i}.log"
                                                                echo "Results" > $filename
                                                                eval $cmd >> $filename 2>&1

                                                                ms=$(cat $filename | grep -oP 'runtime_ms \K[0-9]+')
                                                                (( tot_time += ms))

                                                                empty_work=$(cat $filename | grep -oP 'empty work \K[0-9]+')
                                                                (( tot_empty_work += empty_work))
                                                        done

                                                        avg_time=$((tot_time / trials))
                                                        avg_empty_work=$((tot_empty_work / trials))
                                                        
                                                        printf "${cols_txt}\n" $cnt1 $ds $exec $e $d $dataset $q $c $t $avg_time $avg_empty_work >> $summary_txt
                                                        printf "${cols_csv}\n" $cnt1 $ds $exec $e $d $dataset $q $c $t $avg_time $avg_empty_work >> $summary_csv
                                                        tail -1 $summary_txt
                                                        
                                                        cnt1=`expr $cnt1 + 1`
                                                done
                                        done
                                done 
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
                batch_size=(128)

                for b in "${batch_size[@]}"; do
                        for d in "${delta[@]}"; do
                                for s in "${stickiness[@]}"; do
                                        for t in "${threads[@]}"; do
                                                num_queues=$((t * 2))
                                                cmd="LD_PRELOAD=/usr/lib/x86_64-linux-gnu/libjemalloc.so.2 ./build/astar input/germany.bin 0 320970 MQBucket $t ${num_queues} $b $b $d 64 0"
                                                
                                                tot_time=0
                                                tot_empty_work=0
                                                for ((i = 1; i <= trials; i++)); do
                                                        filename="${log_dir}/${cnt1}_${ds}_results_astar_germany.bin_batch${b}_delta${d}_stick${s}_threads${t}_trial${i}.log"
                                                        echo "Results" > $filename
                                                        eval $cmd >> $filename 2>&1

                                                        ms=$(cat $filename | grep -oP 'runtime_ms \K[0-9]+')
                                                        (( tot_time += ms))

                                                        empty_work=$(cat $filename | grep -oP 'empty work \K[0-9]+')
                                                        (( tot_empty_work += empty_work))
                                                done

                                                avg_time=$((tot_time / trials))
                                                avg_empty_work=$((tot_empty_work / trials))
                                                
                                                printf "${cols_txt}\n" $cnt1 $ds $exec $b $d $dataset $num_queues $s $t $avg_time $avg_empty_work >> $summary_txt
                                                printf "${cols_csv}\n" $cnt1 $ds $exec $b $d $dataset $num_queues $s $t $avg_time $avg_empty_work >> $summary_csv
                                                tail -1 $summary_txt
                                                
                                                cnt1=`expr $cnt1 + 1`
                                        done
                                done
                        done
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
                        for t in "${threads[@]}"; do
                                if [[ $ds == "Linden" ]]; then
                                        cmd="./build/astar input/germany.bin 0 320970 ${ds} $t 0 0 0 0 0 0"
                                else
                                        cmd="LD_PRELOAD=/usr/lib/x86_64-linux-gnu/libjemalloc.so.2 ./build/astar input/germany.bin 0 320970 ${ds} $t 0 0 0 0 0 0"
                                fi

                                tot_time=0
                                tot_empty_work=0

                                for ((i = 1; i <= trials; i++)); do
                                        filename="${log_dir}/${cnt1}_${ds}_results_astar_germany.bin_threads${t}_trial${i}.log"
                                        echo "Results" > $filename
                                        eval $cmd >> $filename 2>&1

                                        ms=$(cat $filename | grep -oP 'runtime_ms \K[0-9]+')
                                        empty_work=$(cat $filename | grep -oP 'empty work \K[0-9]+')

                                        (( tot_time += ms))
                                        (( tot_empty_work += empty_work))
                                done

                                avg_time=$((tot_time / trials))
                                avg_empty_work=$((tot_empty_work / trials))
                                
                                printf "${cols_txt}\n" $cnt1 $ds $exec 0 0 $dataset 0 0 $t $avg_time $avg_empty_work >> $summary_txt
                                printf "${cols_csv}\n" $cnt1 $ds $exec 0 0 $dataset 0 0 $t $avg_time $avg_empty_work >> $summary_csv
                                tail -1 $summary_txt
                                
                                cnt1=`expr $cnt1 + 1`
                        done
                done
        fi
done
