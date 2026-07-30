#!/bin/bash

threads=(1 12 24 48 96)
ds=PIPQ

# ppsp - livejournal
echo "RUNNING LIVEJOURNAL!"
for t in "${threads[@]}"; do
    echo "------------------------------------------------------------"
    echo "RUNNING LIVEJOURNAL - THREADS = $t"
    ./build/lonestar/analytics/cpu/ppsp/ppsp-cpu -startNode 1 -destNode 3000000 -threads $t -algo=${ds} inputs/weighted_inputs/w_1-10_livejournal.gr
    echo
    echo
done

echo
echo
echo

# ppsp - orkut
echo "RUNNING ORKUT!"
for t in "${threads[@]}"; do
    echo "------------------------------------------------------------"
    echo "RUNNING ORKUT - THREADS = $t"
    ./build/lonestar/analytics/cpu/ppsp/ppsp-cpu -startNode 1 -destNode 3000000 -threads $t -algo=${ds} inputs/weighted_inputs/w_1-10_orkut.gr
    echo
    echo
done

echo
echo
echo

# ppsp - roadnetCA
echo "RUNNING ROADNETCA!"
for t in "${threads[@]}"; do
    echo "------------------------------------------------------------"
    echo "RUNNING ROADNETCA - THREADS = $t"
    ./build/lonestar/analytics/cpu/ppsp/ppsp-cpu -startNode 1 -destNode 300000 -threads $t -algo=${ds} inputs/weighted_inputs/w_1-10_roadnetCA.gr
    echo
    echo
done