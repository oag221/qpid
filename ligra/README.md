# Ligra

This is a minimalistic version of the ligra repo that only contains
necessary files for SetCover and DeltaStepping.

`SetCover_MQ.C` contains the code for MultiQueue-based
implementations of greedy approximate setcover.

These versions rely on MultiQueue files from the cps repo, as specified in the Makefile.

# Prerequisite
The executables are built with Clang 16 with OpenCilk 2.1
and the MQ versions require boost >= 1.58.0.

# Build & Run
```
# build
make all

# run the julienne setcover
./runscjulienne.sh

# run the MQ-based setcover
./runscmq.sh

# run the julienne delta-stepping
./runds.sh
```

