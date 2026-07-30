#CC = clang++
CC = ~/opencilk/bin/clang++
CCFLAGS = -mcx16 -O3 -std=c++17 -DNDEBUG
CLFLAGS =
CFLAGS = -DPARLAY_SEQUENTIAL -pthread $(CCFLAGS)
LFLAGS = -lpthread $(CLFLAGS)
GITROOT := $(shell git rev-parse --show-toplevel)
CCFLAGS += -I$(GITROOT) -I$(GITROOT)/parlaylib/include

ifeq (, $(shell which ld.lld))
else
    CLFLAGS += -fuse-ld=lld
endif

BENCH = MIS
OBJS = MIS.o

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

TIME = $(BENCH)Time
CHECK = ../bench/$(BENCH)Check
INCLUDE =

# Make benchmark
$(BENCH) : $(TIME).o $(OBJS) $(CHECK)
	$(CC) $(SKIPHASH_INC) $(SMQ_INC) $(ALL_PQ_INC) -o $@ $(TIME).o $(OBJS) $(LFLAGS) $(ALL_PQ_SRCS)

# Timing Code
$(TIME).o : ../bench/$(TIME).C $(CPS)
	$(CC) $(CFLAGS) $(INCLUDE) -o $@ -c ../bench/$(TIME).C
	
# The check code
$(CHECK) : $(CHECK).C
	make $(BENCH)Check -C ../bench

# object files
%.o : %.C $(REQUIRE) 
	$(CC) $(CFLAGS) $(INCLUDE) -c $< -o $@

$(CPS):
	ln -s ../../../cps/include/*.h .

clean :
	rm -f $(BENCH) *.o

cleanall : clean
	rm -f testInputs*;
	make clean -C ../bench
