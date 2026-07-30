#!/bin/bash

exec_dir=./build
input_dir=../../inputs_astar
startnode=0
endnode=320970

# MQBucket
echo "Results" > results_astar_1_germany_MQBucket.log
for j in {1..10}
do
	${exec_dir}/astar ${input_dir}/germany.bin $startnode $endnode MQBucket 1 4 4 4 11 64 0 >> results_astar_1_germany_MQBucket.log
done
mv path.txt MQBucket_1_germany_path.txt

echo "Results" > results_astar_48_germany_MQBucket.log
for j in {1..10}
do
        ${exec_dir}/astar ${input_dir}/germany.bin $startnode $endnode MQBucket 48 192 4 4 11 64 0 >> results_astar_48_germany_MQBucket.log
done
mv path.txt MQBucket_48_germany_path.txt

# MQ
echo "Results" > results_astar_1_germany_MQ.log
for j in {1..10}
do
        ${exec_dir}/astar ${input_dir}/germany.bin $startnode $endnode MQ 1 4 2 2 >> results_astar_1_germany_MQ.log
done
mv path.txt MQ_1_germany_path.txt

echo "Results" > results_astar_48_germany_MQ.log
for j in {1..10}
do
        ${exec_dir}/astar ${input_dir}/germany.bin $startnode $endnode MQ 48 192 2 2 >> results_astar_48_germany_MQ.log
done
mv path.txt MQ_48_germany_path.txt


# MQ Plain
echo "Results" > results_astar_1_germany_MQPlain.log
for j in {1..10}
do
        ${exec_dir}/astar ${input_dir}/germany.bin $startnode $endnode MQ 1 4 1 1 >> results_astar_1_germany_MQPlain.log
done
mv path.txt MQPlain_1_germany_path.txt

echo "Results" > results_astar_48_germany_MQPlain.log
for j in {1..10}
do
        ${exec_dir}/astar ${input_dir}/germany.bin $startnode $endnode MQ 48 192 2 2 >> results_astar_48_germany_MQPlain.log
done
mv path.txt MQPlain_48_germany_path.txt

# ./build/astar input/germany.bin 0 320970 MQBucket 48 192 4 4 11 64 0


# LD_PRELOAD=/usr/lib/x86_64-linux-gnu/libjemalloc.so.2 ./build/astar input/germany.bin 0 320970 SkipHashPQ 96 4 4 0 1 15

# ./build/astar input/germany.bin 0 320970 Spraylist 96 32 32 0 1 15

# LD_PRELOAD=/usr/lib/x86_64-linux-gnu/libjemalloc.so.2 ./build/astar input/germany.bin 0 320970 SkipHashPQ 96 8 8 1 0 15

"Types: Serial / MQ / MQBucket"
"Usage: %s <inFile> <startNode> <endNode> "
"[qType threadNum queueNum batchPop batchPush delta bucketNum printFull]"

"Types: SkipHashPQ"
"Usage: %s <inFile> <startNode> <endNode> "
"[qType threadNum chunksize num_chunks strict batch delta num_buckets sl_max_levels]"


# LD_PRELOAD=/usr/lib/x86_64-linux-gnu/libjemalloc.so.2 ./build/astar input/germany.bin 0 320970 SkipHashPQ 1 4 4 1 0 15