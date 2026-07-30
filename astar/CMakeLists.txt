cmake_minimum_required(VERSION 3.10)
project(astar)

add_definitions(-DPROFILING -DPROFILING_SERIAL)

include_directories(${Boost_INCLUDE_DIRS})
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_C_FLAGS_RELEASE "${CMAKE_C_FLAGS_RELEASE} -O3")
set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} -O3")

set(CPS_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../cps")
include_directories(${CPS_ROOT})

set(CACHE_LINE_SIZE 64)

set(CPS_SOURCES
    ${CPS_ROOT}/common/fraser.cpp
    ${CPS_ROOT}/common/intset.cpp
    ${CPS_ROOT}/common/skiplist.cpp
    
    # Memory Management & GC
    ${CPS_ROOT}/common/gc/gc.cpp
    ${CPS_ROOT}/common/gc/ptst.cpp

    ${CPS_ROOT}/common/ssalloc.cpp     
    ${CPS_ROOT}/common/measurements.cpp 

    # Linden Data Structure
    ${CPS_ROOT}/include-linden/linden.cpp
    ${CPS_ROOT}/include-linden/linden_common.cpp

    # Spraylist Priority Queue (Prefer .cpp over .c)
    ${CPS_ROOT}/include-spraylist/pqueue.cpp
)
add_library(cps_lib STATIC ${CPS_SOURCES})
target_compile_definitions(cps_lib PRIVATE 
    CACHE_LINE_SIZE=${CACHE_LINE_SIZE} 
    INTEL
    #PROFILING
    #PROFILING_SERIAL
)
target_include_directories(cps_lib PUBLIC
    ${CPS_ROOT}/common
    ${CPS_ROOT}/common/include
    ${CPS_ROOT}/common/atomic_ops
    ${CPS_ROOT}/include-linden
    ${CPS_ROOT}/include-spraylist
    ${CPS_ROOT}/common/gc
)
target_link_libraries(cps_lib PRIVATE pthread rt numa)

add_executable(astar "astar.cpp" "util.h")
# target_compile_definitions(astar PRIVATE CHUNK_POOL)
# target_compile_definitions(astar PRIVATE PROFILING PROFILING_SERIAL)
target_link_libraries(astar
    pthread
    ${Boost_LIBRARIES}
    #mimalloc
    cps_lib)
