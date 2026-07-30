#!/bin/bash

input_dir=../../inputs_pbbs/wag

graphs=(USA LJ TW HYPERH)  
deltas=(16384 32 16 4)
startnodes=(1 1 1 2)

for i in {0..3}
do
	CILK_NWORKERS=48 ./DeltaStepping -nb 64 -delta ${deltas[i]} -src ${startnodes[i]} -rounds 10 ${input_dir}/${graphs[i]}.wag >> results_ds_48_${graphs[i]}_julienne.log
	CILK_NWORKERS=1 ./DeltaStepping -nb 64 -delta ${deltas[i]} -src ${startnodes[i]} -rounds 10 ${input_dir}/${graphs[i]}.wag >> results_ds_1_${graphs[i]}_julienne.log
done
