#!/bin/bash

# usage: bash ./run_rbp_all.sh [ linden | pipq | spray | smq | MQBucket | SkiphashPQ ]

num_params=$(( $# ))
if [[ $num_params == 0 ]]; then
        echo "No data structures specified. Choose among: [ linden | pipq | spray | smq | MQBucket | SkiphashPQ ]. Quitting."
        exit 1
fi

#########################################################
#########################################################
#### Setup directories & output/log files 
#########################################################
#########################################################

input_dir=input
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

# For output configs
cols_txt="%8s %12s %12s %12s %7s %12s %10s %12s %9s %10s %12s %12s %12s %12s %12s"
cols_csv="%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s"

cnt1=10000

#########################################################
#########################################################
#### Alg & dataset configs
#########################################################
#########################################################

threads=(1 12 24 48 96)
trials=3
exec=rbp
dataset=ising

#########################################################
#########################################################
#### Run tests
#########################################################
#########################################################

for cur_ds in "$@"; do
        selected=0
        if [[ $cur_ds == "ALL" || $cur_ds == "SkiphashPQ" ]]; then
                selected=1
                #########################################################
                #########################################################
                #### SkiphashPQ
                #########################################################
                #########################################################
                ds="SkiphashPQ"
                delta_strict=21
                delta_batch=17
                printf "NOTE: for SkipHashPQ, \`n_queues\` for 1 thread is acually always 1 (printed as not 1 for convenience of grouping)\n\n"

                headers="step ds alg exp delta graph n_queues chunk_size threads time(ms) iters updates skips accuracy acc_max"
                printf "${cols_txt}\n" ${headers} >> $summary_txt
                printf "${cols_csv}\n" ${headers} >> $summary_csv
                tail $summary_txt

                # SkipHashPQ specific configs
                chunksize=(512)
                num_lanes=(128)
                exp_type=("batch" "strict")

                for e in "${exp_type[@]}"; do
                        batch_opt=0
                        strict_opt=0
                        if [[ "$e" == "strict" ]]; then
                                strict_opt=1
                                delta=${delta_strict}
                        elif [[ "$e" == "batch" ]]; then
                                batch_opt=1
                                delta=${delta_batch}
                        fi


                        for q in "${num_lanes[@]}"; do
                                for c in "${chunksize[@]}"; do
                                        for t in "${threads[@]}"; do
                                                if [ "$t" -eq 1 ]; then
                                                        n_queues=1
                                                else
                                                        n_queues=$q 
                                                fi

                                                cmd="LD_PRELOAD=/usr/lib/x86_64-linux-gnu/libjemalloc.so.2 ./build/rbp skiphashpq-rbp ising 1000 $t ${n_queues} 0 0 $delta 0 0 0 $c ${batch_opt} ${strict_opt}"

                                                tot_time=0
                                                tot_iters=0
                                                tot_updates=0
                                                tot_skips=0
                                                tot_acc=0
                                                tot_acc_max=0
                                                for ((i = 1; i <= trials; i++)); do
                                                        filename="${log_dir}/${cnt1}_${ds}-${e}_results_astar_germany.bin_chunksize${c}_queues${n_queues}_delta${delta}_threads${t}_trial${i}.log"
                                                        echo "Results" > $filename
                                                        eval $cmd >> $filename 2>&1

                                                        ms=$(cat $filename | grep -oP 'runtime_ms \K[0-9]+')
                                                        iters_=$(cat $filename | grep -oP 'totalIters = \K[0-9]+')
                                                        updates_=$(cat $filename | grep -oP 'totalUpdates = \K[0-9]+')
                                                        skips_=$(cat $filename | grep -oP 'totalSkips = \K[0-9]+')
                                                        acc_=$(cat $filename | grep -oP 'Accuracy:\K[0-9\.]+')
                                                        acc_max_=$(cat $filename | grep -oP 'AccuracyMax:\K[0-9\.]+')

                                                        (( tot_time += ms))
                                                        (( tot_iters += iters_))
                                                        (( tot_updates += updates_))
                                                        (( tot_skips += skips_))
                                                        tot_acc=$(echo "$tot_acc + ${acc_:-0}" | bc)
                                                        tot_acc_max=$(echo "$tot_acc_max + ${acc_max_:-0}" | bc)
                                                done

                                                avg_time=$((tot_time / trials))
                                                iters=$(( tot_iters / trials))
                                                updates=$(( tot_updates / trials))
                                                skips=$(( tot_skips / trials))
                                                acc=$(echo "scale=6; $tot_acc / $trials" | bc)
                                                acc_max=$(echo "scale=6; $tot_acc_max / $trials" | bc)
                                                
                                                printf "${cols_txt}\n" $cnt1 $ds $exec $e $delta $dataset $q $c $t $avg_time $iters $updates $skips $acc $acc_max >> $summary_txt
                                                printf "${cols_csv}\n" $cnt1 $ds $exec $e $delta $dataset $q $c $t $avg_time $iters $updates $skips $acc $acc_max >> $summary_csv
                                                tail -1 $summary_txt
                                                
                                                ((cnt1++))
                                        done
                                done
                        done 
                done
        fi
        if [[ $cur_ds == "ALL" || $cur_ds == "MQBucket" ]]; then
                selected=1
                #########################################################
                #########################################################
                #### MQBucket
                #########################################################
                #########################################################
                ds="MQBucket"
                delta=(7)

                headers_mqbucket="step ds alg batch_size delta graph n_queues stick threads time(ms) iters updates skips accuracy acc_max"
                printf "\n${cols_txt}\n" ${headers_mqbucket} >> $summary_txt
                printf "${cols_csv}\n" ${headers_mqbucket} >> $summary_csv
                tail -1 $summary_txt

                prefetch=0
                stickiness=(8)
                batch_size=(1 128)

                for b in "${batch_size[@]}"; do
                        for d in "${delta[@]}"; do
                                for s in "${stickiness[@]}"; do
                                        for t in "${threads[@]}"; do
                                                num_queues=$((t * 2))
                                                cmd="LD_PRELOAD=/usr/lib/x86_64-linux-gnu/libjemalloc.so.2 ./build/rbp bucket ising 1000 $t ${num_queues} $b $b $d 64 0 $s 0 0 0"
                                                
                                                tot_time=0
                                                tot_iters=0
                                                tot_updates=0
                                                tot_skips=0
                                                tot_acc=0
                                                tot_acc_max=0
                                                for ((i = 1; i <= trials; i++)); do
                                                        filename="${log_dir}/${cnt1}_${ds}_results_astar_germany.bin_batch${b}_delta${d}_stick${s}_threads${t}_trial${i}.log"
                                                        echo "Results" > $filename
                                                        eval $cmd >> $filename 2>&1

                                                        ms=$(cat $filename | grep -oP 'runtime_ms \K[0-9]+')
                                                        iters_=$(cat $filename | grep -oP 'totalIters = \K[0-9]+')
                                                        updates_=$(cat $filename | grep -oP 'totalUpdates = \K[0-9]+')
                                                        skips_=$(cat $filename | grep -oP 'totalSkips = \K[0-9]+')
                                                        acc_=$(cat $filename | grep -oP 'Accuracy:\K[0-9\.]+')
                                                        acc_max_=$(cat $filename | grep -oP 'AccuracyMax:\K[0-9\.]+')
                                                        
                                                        (( tot_time += ms))
                                                        (( tot_iters += iters_))
                                                        (( tot_updates += updates_))
                                                        (( tot_skips += skips_))
                                                        tot_acc=$(echo "$tot_acc + ${acc_:-0}" | bc)
                                                        tot_acc_max=$(echo "$tot_acc_max + ${acc_max_:-0}" | bc)
                                                done

                                                avg_time=$((tot_time / trials))
                                                iters=$(( tot_iters / trials))
                                                updates=$(( tot_updates / trials))
                                                skips=$(( tot_skips / trials))
                                                acc=$(echo "scale=6; $tot_acc / $trials" | bc)
                                                acc_max=$(echo "scale=6; $tot_acc_max / $trials" | bc)
                                                
                                                printf "${cols_txt}\n" $cnt1 $ds $exec $b $d $dataset $num_queues $s $t $avg_time $iters $updates $skips $acc $acc_max >> $summary_txt
                                                printf "${cols_csv}\n" $cnt1 $ds $exec $b $d $dataset $num_queues $s $t $avg_time $iters $updates $skips $acc $acc_max >> $summary_csv
                                                tail -1 $summary_txt
                                                
                                                ((cnt1++))
                                        done
                                done
                        done
                done
        fi
        if [[ $cur_ds == "ALL" || $cur_ds == "pipq"  || $cur_ds == "linden" || $cur_ds == "spray" || $cur_ds == "smq" ]]; then
                selected=1
                #########################################################
                #########################################################
                #### PIPQ, SMQ, Linden, Spray
                #########################################################
                #########################################################
                if [[ $cur_ds == "ALL" ]]; then
                        pqs=("linden" "smq" "pipq" "spray")
                else
                        pqs=($cur_ds)
                fi

                headers_compet="step ds alg exp delta graph n_queues chunk_size threads time(ms) iters updates skips accuracy acc_max"
                printf "${cols_txt}\n" ${headers_compet} >> $summary_txt
                printf "${cols_csv}\n" ${headers_compet} >> $summary_csv
                tail $summary_txt

                for ds in "${pqs[@]}"; do
                        for t in "${threads[@]}"; do
                                if [[ $ds == "Linden" ]]; then
                                        cmd="./build/rbp ${ds} ising 1000 $t 0 0 0 0 0 0 0 0 0 0"
                                else
                                        cmd="LD_PRELOAD=/usr/lib/x86_64-linux-gnu/libjemalloc.so.2 ./build/rbp ${ds} ising 1000 $t 0 0 0 0 0 0 0 0 0 0"
                                fi

                                tot_time=0
                                tot_iters=0
                                tot_updates=0
                                tot_skips=0
                                tot_acc=0
                                tot_acc_max=0
                                for ((i = 1; i <= trials; i++)); do
                                        filename="${log_dir}/${cnt1}_${ds}_results_astar_germany.bin_threads${t}_trial${i}.log"
                                        echo "Results" > $filename
                                        eval $cmd >> $filename 2>&1

                                        ms=$(cat $filename | grep -oP 'runtime_ms \K[0-9]+')
                                        iters_=$(cat $filename | grep -oP 'totalIters = \K[0-9]+')
                                        updates_=$(cat $filename | grep -oP 'totalUpdates = \K[0-9]+')
                                        skips_=$(cat $filename | grep -oP 'totalSkips = \K[0-9]+')
                                        acc_=$(cat $filename | grep -oP 'Accuracy:\K[0-9\.]+')
                                        acc_max_=$(cat $filename | grep -oP 'AccuracyMax:\K[0-9\.]+')

                                        (( tot_time += ms))
                                        (( tot_iters += iters_))
                                        (( tot_updates += updates_))
                                        (( tot_skips += skips_))
                                        tot_acc=$(echo "$tot_acc + ${acc_:-0}" | bc)
                                        tot_acc_max=$(echo "$tot_acc_max + ${acc_max_:-0}" | bc)
                                done

                                avg_time=$((tot_time / trials))
                                iters=$(( tot_iters / trials))
                                updates=$(( tot_updates / trials))
                                skips=$(( tot_skips / trials))
                                acc=$(echo "scale=6; $tot_acc / $trials" | bc)
                                acc_max=$(echo "scale=6; $tot_acc_max / $trials" | bc)
                                
                                printf "${cols_txt}\n" $cnt1 $ds $exec 0 0 $dataset 0 0 $t $avg_time $iters $updates $skips $acc $acc_max >> $summary_txt
                                printf "${cols_csv}\n" $cnt1 $ds $exec 0 0 $dataset 0 0 $t $avg_time $iters $updates $skips $acc $acc_max >> $summary_csv
                                tail -1 $summary_txt
                                
                                ((cnt1++))
                        done
                done
        fi
        if [[ $selected -eq 0 ]]; then 
                echo "Error: '${cur_ds}' does not match any selections."
                echo "Choose among: [ linden | pipq | spray | smq | MQBucket | SkiphashPQ ]"
        fi
done
