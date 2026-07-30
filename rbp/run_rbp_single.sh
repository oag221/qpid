#!/bin/bash

# Default values
CHUNKSIZE=""
DELTA=""
QUEUES=""

# 1. Parse command line parameters
while getopts "c:d:q:" opt; do
  case $opt in
    c) CHUNKSIZE="$OPTARG" ;;
    d) DELTA="$OPTARG" ;;
    q) QUEUES="$OPTARG" ;;
    *) echo "Usage: $0 -c <val> -d <val> -q <val>"; exit 1 ;;
  esac
done

# Check if arguments were provided
if [[ -z "$CHUNKSIZE" || -z "$DELTA" || -z "$QUEUES" ]]; then
    echo "Error: Missing parameters -c or -d or -q"
    exit 1
fi

# 2. Run the command
# Replace './my_command -s $S_VAL -c $C_VAL' with your actual executable
COMMAND_OUTPUT=$(LD_PRELOAD=/usr/lib/x86_64-linux-gnu/libjemalloc.so.2 ./build/rbp skiphashpq-rbp ising 1000 96 "$QUEUES" 1 1 "$DELTA" 64 0 0 "$CHUNKSIZE" 2>&1)

# Usage: ./build/rbp <algorithm> <mrf> <size> [<threads>] [<queues>] [<batchPop>] [<batchPush>] [<delta>] [<buckets>] [<usePrefetch>] [<stickiness>] [<chunksize>] [<batch_ins>] [<strict>]

# LD_PRELOAD=/usr/lib/x86_64-linux-gnu/libjemalloc.so.2 ./build/rbp skiphashpq-rbp ising 1000 96 64 0 0 16 0 0 0 64 0 0

# LD_PRELOAD=/usr/lib/x86_64-linux-gnu/libjemalloc.so.2 ./build/rbp bucket ising 1000 96 192 128 128 7 64 1 1 64 0 0 

t=12
q=$((2 * t))
./build/rbp bucket ising 1000 $t $q 128 128 7 64 0 16 1 1

#echo $COMMAND_OUTPUT

# 3. Extract information using AWK
# This logic saves the most recent match for each pattern
echo "$COMMAND_OUTPUT" | awk '
/totalIters =/    { iters = $3 }
/totalUpdates =/  { updates = $3 }
/totalSkips =/    { skips = $3 }
/Accuracy:/       { split($1, a, ":"); acc = a[2] }
/AccuracyMax:/    { split($1, a, ":"); accMax = a[2] }
/The first results are/ { res1 = $5; res2 = $7 }
/runtime_ms/      { runtime = $2 }

END {
    printf "\n--- Process Results ---\n"
    printf "%-15s : %s\n", "Total Iters", iters
    printf "%-15s : %s\n", "Total Updates", updates
    printf "%-15s : %s\n", "Total Skips", skips
    printf "%-15s : %s\n", "Accuracy", acc
    printf "%-15s : %s\n", "Max Accuracy", accMax
    printf "%-15s : %s, %s\n", "First Results", res1, res2
    printf "%-15s : %s ms\n", "Runtime", runtime
    printf "-----------------------\n"
}'