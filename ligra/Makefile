ifdef LONG
INTT = -DLONG
endif

ifdef EDGELONG
INTE = -DEDGELONG
endif

ifdef PD
PD = -DPD
endif

ifdef BYTE
CODE = -DBYTE
else ifdef NIBBLE
CODE = -DNIBBLE
else
CODE = -DBYTERLE
endif

# Use clang to compile
#PCC = clang++
PCC = ~/opencilk/bin/clang++
PCFLAGS = -std=c++17 -fopencilk -O3 -DCILK $(INTT) $(INTE) $(CODE) $(PD) $(FLAGS)
PCFLAGS_MQ = -std=c++20 -fopencilk -O3 -DCILK -DCACHE_LINE_SIZE=64 -DINTEL -DSC -lnuma $(INTT) $(INTE) $(CODE) $(PD) $(FLAGS) #-DPROFILING
#PLFLAGS = 

COMMON= ligra.h edgeMap_utils.h index_map.h sequence.h maybe.h binary_search.h graph.h compressedVertex.h vertex.h utils.h IO.h parallel.h gettime.h quickSort.h blockRadixSort.h transpose.h parseCommandLine.h byte.h byteRLE.h nibble.h byte-pd.h byteRLE-pd.h nibble-pd.h vertexSubset.h encoder.C bucket.h counting_sort.h dyn_arr.h edgeMapReduce.h histogram.h sequentialHT.h

CPS= MultiQueue.h MultiBucketQueue.h BucketStructs.h

# --- DATA STRUCTURE INTEGRATION ---

# 1. Directories
SPRAY_DIR   = ../cps/include-spraylist
LINDEN_DIR  = ../cps/include-linden
PIPQ_DIR    = ../cps/include-pipq
COMMON_DIR  = ../cps/common

# 2. Include Flags
# Note: Common subdirectories are added because headers often include "gc.h" directly
SPRAY_INC   = -I$(SPRAY_DIR) -I$(COMMON_DIR) -I$(COMMON_DIR)/include -I$(COMMON_DIR)/gc -I$(COMMON_DIR)/atomic_ops
LINDEN_INC  = -I$(LINDEN_DIR)
PIPQ_INC    = -I$(PIPQ_DIR)

# Combined Include Flag
ALL_PQ_INC  = $(SPRAY_INC) $(LINDEN_INC) $(PIPQ_INC)

# 3. Source Files (Implementations)
# These must be compiled along with your main program
COMMON_SRCS = $(COMMON_DIR)/ssalloc.cpp $(COMMON_DIR)/gc/gc.cpp $(COMMON_DIR)/gc/ptst.cpp $(COMMON_DIR)/skiplist.cpp $(COMMON_DIR)/fraser.cpp $(COMMON_DIR)/intset.cpp
SPRAY_SRCS  = $(SPRAY_DIR)/pqueue.cpp
LINDEN_SRCS = $(LINDEN_DIR)/linden.cpp $(LINDEN_DIR)/linden_common.cpp

# Combined Source Variable
ALL_PQ_SRCS = $(COMMON_SRCS) $(SPRAY_SRCS) $(LINDEN_SRCS)

SKIPHASH_INC= -I../cps/include-skiphashpq/

SMQ_INC= -I../cps/include-smq/

BKTMQ= SetCover_MQ

JUL= SetCover DeltaStepping

ALL= DeltaStepping SetCover SetCover_MQ

all: $(BKTMQ) $(JUL)

SetCover: SetCover.C $(COMMON)
	$(PCC) $(PCFLAGS) -o $@ $<

DeltaStepping: DeltaStepping.C $(COMMON)
	$(PCC) $(PCFLAGS) -o $@ $<

# SetCover_MQ: SetCover_MQ.C $(COMMON) $(CPS)
# 	$(PCC) $(PCFLAGS_MQ) -MMD $(SKIPHASH_INC) $(SMQ_INC) -o $@ $<

SetCover_MQ: SetCover_MQ.C $(COMMON) $(CPS)
	$(PCC) $(PCFLAGS_MQ) -MMD $(SKIPHASH_INC) $(SMQ_INC) $(ALL_PQ_INC) -o $@ $< $(ALL_PQ_SRCS)

#clang++ -MMD -std=c++20 -O3 $(INTT) $(INTE) $(CODE) $(PD) $(SKIPHASH_INC) -o $@ $<

$(COMMON):
	ln -s ./ligra/$@ .

$(CPS):
	ln -s ../cps/include/$@ .

.PHONY : clean

clean :
	rm -f *.o $(BKTMQ) $(JUL)

cleansrc :
	rm -f *.o $(BKTMQ) $(JUL)
	rm $(COMMON)
	rm $(CPS)

