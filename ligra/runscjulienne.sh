#!/bin/bash

input_dir=../../inputs_pbbs/ag

graphs=(USA LJ TW HYPERH)

for i in {0..3}
do
    for j in {1..10}
    do
        CILK_NWORKERS=48 ./SetCover -nb 64 -rounds 0 ${input_dir}/${graphs[i]}.ag >> results_sc_48_${graphs[i]}_julienne.log
	CILK_NWORKERS=1 ./SetCover -nb 64 -rounds 0 ${input_dir}/${graphs[i]}.ag >> results_sc_1_${graphs[i]}_julienne.log
    done
done


