#!/bin/bash

if [[ $# = 0 ]]; then
        echo "Must provide data structrues to test. Usage: 'bash ./run_bfs_all [ALL | QPID | MBQ | PIPQ | Linden | Spraylist | SMQ]'"
        exit 1
fi

#########################################################
#########################################################
#### Setup directories & output/log files 
#########################################################
#########################################################

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
headers="step ds duration exp prefill dist n_buckets bucket_step ins n_queues chunk_size threads thpt"
cols_txt="%8s %12s %9s %7s %10s %13s %10s %12s %6s %9s %11s %8s %10s"
cols_csv="%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s"

printf "${cols_txt}\n" ${headers} >> $summary_txt
printf "${cols_csv}\n" ${headers} >> $summary_csv

cnt1=10000

#########################################################
#########################################################
#### Alg & dataset configs
#########################################################
#########################################################

threads=(1 12 24 48 96)
duration=3 #seconds
trials=3
prefill=(1000000) # 100000

# workload setup
ins_perc=(50)
dist=(flat cubic)
n_buckets=(15 100 1000) # active buckets AT A TIME #!
bucket_step=(1)

print_array() {
  local str_prefix="$1"
  local -n arr_ref="$2"  # Create a reference to the array name passed as $2

  # Prefix every element with ", "
  local joined=$(printf ", %s" "${arr_ref[@]}")
  
  # Print the string, a colon, and the joined array (stripping the first 2 characters ", ")
  echo "${str_prefix}: ${joined:2}"
}

echo "================================================"
echo "SETTINGS:"
echo "================================================"
print_array "Threads" threads
echo "Duration: $duration"
echo "Trials: $trials"
print_array "Prefill" prefill
print_array "Insert Percentage" ins_perc
print_array "Distribution" dist
print_array "Num Active Buckets" n_buckets
print_array "Bucket Step" bucket_step
echo "================================================"

# Print headers
tail $summary_txt

#########################################################
#########################################################
#### Run tests
#########################################################
#########################################################

for cur_ds in "$@"; do
        if [[ $cur_ds == "ALL" || $cur_ds == "QPID" || $cur_ds == "QPID_STRICT" ]]; then
                #########################################################
                #########################################################
                #### QPID
                #########################################################
                #########################################################
                
                if [[ $cur_ds == "QPID_STRICT" ]]; then
                        ds="QPID-STRICT"
                        exp_type=strict
                        exec="./build/qpid-strict-exe"
                else
                        ds="QPID-BATCH"
                        exp_type=batch
                        exec="./build/qpid-exe"
                fi
                
                # n_queues=(8 16 32 64 128)
                # chunk_size=(8 16 32 64 128)

                n_queues=(8 32 128)
                chunk_size=(8 32 128)

                for p in "${prefill[@]}"; do
                        for y in "${dist[@]}"; do
                                for b in "${n_buckets[@]}"; do
                                        for u in "${bucket_step[@]}"; do
                                                for i in "${ins_perc[@]}"; do
                                                        for n_q in "${n_queues[@]}"; do
                                                                for c in "${chunk_size[@]}"; do
                                                                        for t in "${threads[@]}"; do
                                                                                if [ "$t" -eq 1 ]; then
                                                                                        q=1
                                                                                else
                                                                                        q=$n_q
                                                                                fi

                                                                                cmd="${exec} -d ${duration} -p ${p} -y ${y} -b ${b} -u ${u} -i ${i} -q ${q} -c ${c} -t ${t}" 

                                                                                tot_thpt=0
                                                                                for ((j = 1; j <= trials; j++)); do
                                                                                        filename="${log_dir}/${cnt1}_${ds}-${exp_type}_results_prefill${p}_dist${y}_ins${i}_q${q}_c${c}_threads${t}_trial${j}.log"
                                                                                        echo "Results" > $filename
                                                                                        echo "$cmd" >> $filename
                                                                                        eval $cmd >> $filename 2>&1

                                                                                        thpt=$(awk '/Throughput:/ {print $2}' "$filename")

                                                                                        (( tot_thpt += thpt ))
                                                                                done

                                                                                avg_thpt=$(( tot_thpt / trials ))

                                                                                printf "${cols_txt}\n" $cnt1 $ds $duration $exp_type $p $y $b $u $i $n_q $c $t ${avg_thpt} >> $summary_txt
                                                                                printf "${cols_csv}\n" $cnt1 $ds $duration $exp_type $p $y $b $u $i $n_q $c $t ${avg_thpt} >> $summary_csv
                                                                                tail -1 $summary_txt

                                                                                cnt1=`expr $cnt1 + 1`
                                                                        done
                                                                done
                                                        done
                                                done
                                        done
                                done
                        done
                done
        fi
        if [[ $cur_ds == "ALL" || $cur_ds == "MBQ" ]]; then
                #########################################################
                #########################################################
                #### MBQ
                #########################################################
                #########################################################

                ds="MBQ"
                exec="./build/mbq-exe"
                #printf "NOTE: for QPID, \`n_queues\` for 1 thread is acually always 1 (printed as not 1 for convenience of grouping)\n"

                exp_type=batch

                chunk_size=(8 32 128) # i.e., batch size

                for p in "${prefill[@]}"; do
                        for y in "${dist[@]}"; do
                                for b in "${n_buckets[@]}"; do
                                        for u in "${bucket_step[@]}"; do
                                                for i in "${ins_perc[@]}"; do
                                                        for c in "${chunk_size[@]}"; do
                                                                for t in "${threads[@]}"; do
                                                                        q=$(( 2 * t ))

                                                                        cmd="${exec} -d ${duration} -p ${p} -y ${y} -b ${b} -u ${u} -i ${i} -q ${q} -c ${c} -t ${t}" 

                                                                        tot_thpt=0
                                                                        for ((j = 1; j <= trials; j++)); do
                                                                                filename="${log_dir}/${cnt1}_${ds}-${exp_type}_results_prefill${p}_dist${y}_ins${i}_q${q}_c${c}_threads${t}_trial${j}.log"
                                                                                echo "Results" > $filename
                                                                                echo "$cmd" >> $filename
                                                                                eval $cmd >> $filename 2>&1

                                                                                thpt=$(awk '/Throughput:/ {print $2}' "$filename")

                                                                                (( tot_thpt += thpt ))
                                                                        done

                                                                        avg_thpt=$(( tot_thpt / trials ))

                                                                        printf "${cols_txt}\n" $cnt1 $ds $duration $exp_type $p $y $b $u $i $q $c $t ${avg_thpt} >> $summary_txt
                                                                        printf "${cols_csv}\n" $cnt1 $ds $duration $exp_type $p $y $b $u $i $q $c $t ${avg_thpt} >> $summary_csv
                                                                        tail -1 $summary_txt

                                                                        cnt1=`expr $cnt1 + 1`
                                                                done
                                                        done
                                                done
                                        done
                                done
                        done
                done
        fi
        if [[ $cur_ds == "ALL" || $cur_ds == "SMQ" ]]; then
                #########################################################
                #########################################################
                #### SMQ
                #########################################################
                #########################################################

                ds="SMQ"
                exec="./build/smq-exe"
                exp_type=-

                chunk_size=(8 32 128) # i.e., steal size

                for p in "${prefill[@]}"; do
                        for y in "${dist[@]}"; do
                                for b in "${n_buckets[@]}"; do
                                        for u in "${bucket_step[@]}"; do
                                                for i in "${ins_perc[@]}"; do
                                                        for c in "${chunk_size[@]}"; do
                                                                for t in "${threads[@]}"; do

                                                                        cmd="${exec} -d ${duration} -p ${p} -y ${y} -b ${b} -u ${u} -i ${i} -c ${c} -t ${t}" 

                                                                        tot_thpt=0
                                                                        for ((j = 1; j <= trials; j++)); do
                                                                                filename="${log_dir}/${cnt1}_${ds}-${exp_type}_results_prefill${p}_dist${y}_ins${i}_steal${c}_threads${t}_trial${j}.log"
                                                                                echo "Results" > $filename
                                                                                echo "$cmd" >> $filename
                                                                                eval $cmd >> $filename 2>&1

                                                                                thpt=$(awk '/Throughput:/ {print $2}' "$filename")

                                                                                (( tot_thpt += thpt ))
                                                                        done

                                                                        avg_thpt=$(( tot_thpt / trials ))

                                                                        printf "${cols_txt}\n" $cnt1 $ds $duration $exp_type $p $y $b $u $i - $c $t ${avg_thpt} >> $summary_txt
                                                                        printf "${cols_csv}\n" $cnt1 $ds $duration $exp_type $p $y $b $u $i - $c $t ${avg_thpt} >> $summary_csv
                                                                        tail -1 $summary_txt

                                                                        cnt1=`expr $cnt1 + 1`
                                                                done
                                                        done
                                                done
                                        done
                                done
                        done
                done
        fi
        if [[ $cur_ds == "ALL" || $cur_ds == "PIPQ"  || $cur_ds == "Linden" || $cur_ds == "Spraylist" ]]; then
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

                q="-"
                c="-"
                exp_type="-"
                if [[ $cur_ds == "Linden" ]]; then
                        exec="./build/linden-exe"
                elif [[ $cur_ds == "PIPQ" ]]; then
                        exec="./build/pipq-exe"
                elif [[ $cur_ds == "Spraylist" ]]; then
                        exec="./build/spraylist-exe"
                fi
                

                for ds in "${pqs[@]}"; do
                        for p in "${prefill[@]}"; do
                                for y in "${dist[@]}"; do
                                        for b in "${n_buckets[@]}"; do
                                                for u in "${bucket_step[@]}"; do
                                                        for i in "${ins_perc[@]}"; do
                                                                for t in "${threads[@]}"; do

                                                                        cmd="${exec} -d ${duration} -p ${p} -y ${y} -b ${b} -u ${u} -i ${i} -t ${t}" 

                                                                        tot_thpt=0
                                                                        for ((j = 1; j <= trials; j++)); do
                                                                                filename="${log_dir}/${cnt1}_${ds}_results_prefill${p}_dist${y}_ins${i}_threads${t}_trial${j}.log"
                                                                                echo "Results" > $filename
                                                                                echo "$cmd" >> $filename
                                                                                eval $cmd >> $filename 2>&1

                                                                                thpt=$(awk '/Throughput:/ {print $2}' "$filename")

                                                                                (( tot_thpt += thpt ))
                                                                        done

                                                                        avg_thpt=$(( tot_thpt / trials ))

                                                                        printf "${cols_txt}\n" $cnt1 $ds $duration $exp_type $p $y $b $u $i $q $c $t ${avg_thpt} >> $summary_txt
                                                                        printf "${cols_csv}\n" $cnt1 $ds $duration $exp_type $p $y $b $u $i $q $c $t ${avg_thpt} >> $summary_csv
                                                                        tail -1 $summary_txt

                                                                        cnt1=`expr $cnt1 + 1`
                                                                done
                                                        done
                                                done
                                        done
                                done
                        done
                done
        fi
done
