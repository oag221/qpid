cmake_minimum_required(VERSION 3.10)
project(rbp)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_FLAGS_RELEASE "-O3")
set(CMAKE_CXX_FLAGS_DEBUG "-g -O3")

# add_definitions(-DPROFILING -D_SERIAL_PROFILING)

include_directories(${CMAKE_CURRENT_SOURCE_DIR}/../cps/include)
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/../cps/include-skiphashpq)
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/../cps/include-smq)
# # include_directories(${CMAKE_CURRENT_SOURCE_DIR}/../cps/common)
# # include_directories(${CMAKE_CURRENT_SOURCE_DIR}/../cps/common/include/)
# include_directories(${CMAKE_CURRENT_SOURCE_DIR}/../cps/include-pipq)
# include_directories(${CMAKE_CURRENT_SOURCE_DIR}/../cps/include-spraylist)
# include_directories(${CMAKE_CURRENT_SOURCE_DIR}/../cps/include-linden)

set(CPS_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../cps")
include_directories(${CPS_ROOT})

set(CPS_SOURCES
    # Common / Core
    ${CPS_ROOT}/common/fraser.cpp
    ${CPS_ROOT}/common/intset.cpp
    ${CPS_ROOT}/common/skiplist.cpp
    
    # Memory Management & GC
    ${CPS_ROOT}/common/gc/gc.cpp
    ${CPS_ROOT}/common/gc/ptst.cpp
    
    # Note: These two were dependencies in the old Makefile. 
    # Please verify they exist in 'common/' or 'common/src'. 
    # If they are missing, the build will fail.
    ${CPS_ROOT}/common/ssalloc.cpp     
    ${CPS_ROOT}/common/measurements.cpp 

    # Linden Data Structure
    ${CPS_ROOT}/include-linden/linden.cpp
    ${CPS_ROOT}/include-linden/linden_common.cpp

    # Spraylist Priority Queue
    ${CPS_ROOT}/include-spraylist/pqueue.cpp
)

add_library(cps_lib_rbp STATIC ${CPS_SOURCES})
target_compile_definitions(cps_lib_rbp PRIVATE RBP)
target_compile_definitions(cps_lib_rbp PRIVATE ORDER_DECREASING)
target_compile_definitions(cps_lib_rbp PRIVATE CHUNK_POOL)

target_compile_definitions(cps_lib_rbp PRIVATE 
    CACHE_LINE_SIZE=64
    INTEL
    # PROFILING_SERIAL
    # PROFILING
)

target_include_directories(cps_lib_rbp PUBLIC
    ${CPS_ROOT}/common
    ${CPS_ROOT}/common/include
    ${CPS_ROOT}/common/atomic_ops
    ${CPS_ROOT}/include-linden
    ${CPS_ROOT}/include-spraylist
    ${CPS_ROOT}/common/gc
)
target_link_libraries(cps_lib_rbp PRIVATE pthread rt)

file(GLOB SOURCES
    "edge.h"
    "examples_mrf.h"
    "message.h"
    "mrf.h"
    "relaxed-rbp.h"
    "residual_bp.h"
    "utils.h"
    "heap_rbp.h"
    "bucket_rbp.h"
    "edge.cpp"
    "examples_mrf.cpp"
    "main.cpp"
    "message.cpp"
    "mrf.cpp"
    "residual_bp.cpp"
    "relaxed-rbp.cpp"
    "heap_rbp.cpp"
    "bucket_rbp.cpp"
    "stm_rbp.cpp"
    "SMQ_rbp.cpp"
    "compet_rbp.cpp"
)

add_executable(rbp ${SOURCES})
#target_compile_definitions(rbp PRIVATE LOG_BUCKETS)
target_compile_definitions(rbp PRIVATE RBP)
target_compile_definitions(rbp PRIVATE ORDER_DECREASING)
# target_compile_definitions(rbp PRIVATE PROFILING PROFILING_SERIAL)
#target_link_libraries(rbp pthread ${Boost_LIBRARIES} mimalloc numa cps_lib_rbp)
target_link_libraries(rbp pthread ${Boost_LIBRARIES} numa cps_lib_rbp)
#target_link_libraries(rbp pthread ${Boost_LIBRARIES})
