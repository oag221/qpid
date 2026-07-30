# 1. Set the minimum version of CMake required
cmake_minimum_required(VERSION 3.12)

# 2. Name your project and specify it uses C++
project(Microbenchmark VERSION 1.0 LANGUAGES CXX)

# 3. Specify the C++ standard
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 4. Resolve the include path properly using the HOME environment variable
set(CPS_INCLUDE_DIR "$ENV{HOME}/mbq/cps/")

# 5. Define your executables
add_executable(qpid-exe main.cpp)
add_executable(qpid-strict-exe main.cpp)
add_executable(mbq-exe main.cpp)
add_executable(pipq-exe main.cpp)
add_executable(spraylist-exe main.cpp
    "${CPS_INCLUDE_DIR}/include-spraylist/pqueue.cpp"
    "${CPS_INCLUDE_DIR}/common/intset.cpp"
    "${CPS_INCLUDE_DIR}/common/skiplist.cpp"
    "${CPS_INCLUDE_DIR}/common/fraser.cpp"
)
add_executable(linden-exe main.cpp
    "${CPS_INCLUDE_DIR}/include-linden/linden.cpp"
    "${CPS_INCLUDE_DIR}/include-linden/linden_common.cpp"
    # "${CPS_INCLUDE_DIR}/common/fraser.cpp"
    # "${CPS_INCLUDE_DIR}/common/skiplist.cpp"
    "${CPS_INCLUDE_DIR}/common/gc/gc.cpp"
    "${CPS_INCLUDE_DIR}/common/gc/ptst.cpp"
)
add_executable(smq-exe main.cpp)

# 6. Provide the paths to your include files
target_include_directories(qpid-exe PRIVATE "${CPS_INCLUDE_DIR}")
target_include_directories(qpid-strict-exe PRIVATE "${CPS_INCLUDE_DIR}")
target_include_directories(mbq-exe PRIVATE "${CPS_INCLUDE_DIR}")
target_include_directories(pipq-exe PRIVATE "${CPS_INCLUDE_DIR}")
target_include_directories(spraylist-exe PRIVATE "${CPS_INCLUDE_DIR}")
target_include_directories(linden-exe PRIVATE "${CPS_INCLUDE_DIR}")
target_include_directories(smq-exe PRIVATE "${CPS_INCLUDE_DIR}")

# 7. Apply compiler options
target_compile_options(qpid-exe PRIVATE -fpermissive -O3 -march=native)
target_compile_options(qpid-strict-exe PRIVATE -fpermissive -O3 -march=native)
target_compile_options(mbq-exe PRIVATE -fpermissive -O3 -march=native)
target_compile_options(pipq-exe PRIVATE -fpermissive -O3 -march=native)
target_compile_options(spraylist-exe PRIVATE -fpermissive -O3 -march=native)
target_compile_options(linden-exe PRIVATE -fpermissive -O3 -march=native)
target_compile_options(smq-exe PRIVATE -fpermissive -O3 -march=native)

# 8. Modern CMake conditional compilation flags
target_compile_definitions(qpid-exe PRIVATE QPID)
#target_compile_definitions(qpid-exe PRIVATE QPID PROFILE)
target_compile_definitions(qpid-strict-exe PRIVATE QPID QPID_STRICT)
target_compile_definitions(mbq-exe PRIVATE MBQ)
target_compile_definitions(pipq-exe PRIVATE PIPQ)
target_compile_definitions(spraylist-exe PRIVATE SPRAYLIST)
target_compile_definitions(linden-exe PRIVATE LINDEN
    CACHE_LINE_SIZE=64 
    INTEL)
target_compile_definitions(smq-exe PRIVATE SMQ)

# ==========================================
# 9. Link the Linux NUMA library to pipq
# ==========================================
target_link_libraries(pipq-exe PRIVATE numa)