# add_executable(ppsp-cpu ppsp.cpp)
# add_dependencies(apps ppsp-cpu)
# target_link_libraries(ppsp-cpu PRIVATE Galois::shmem lonestar)
# include_directories(${CMAKE_CURRENT_SOURCE_DIR}/../../../../../cps)
# install(TARGETS ppsp-cpu DESTINATION "${CMAKE_INSTALL_BINDIR}" COMPONENT apps EXCLUDE_FROM_ALL)

# add_test_scale(small1 ppsp-cpu "${BASEINPUT}/reference/structured/rome99.gr" -delta 8)
# add_test_scale(small2 ppsp-cpu "${BASEINPUT}/scalefree/rmat10.gr" -delta 8)

# -----------------------------------------------------------------------------
# 1. Setup Paths & Legacy Configuration
# -----------------------------------------------------------------------------
# Define the root of the CPS library relative to this CMake file
set(CPS_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../../../../cps")
include_directories(${CPS_ROOT})

# Detect Cache Line Size (Required for Linden/Spraylist performance)
if(UNIX AND NOT APPLE)
    execute_process(COMMAND getconf LEVEL1_DCACHE_LINESIZE
                    OUTPUT_VARIABLE CACHE_LINE_SIZE
                    OUTPUT_STRIP_TRAILING_WHITESPACE)
    if(NOT CACHE_LINE_SIZE)
        set(CACHE_LINE_SIZE 64)
    endif()
else()
    set(CACHE_LINE_SIZE 64)
endif()

# -----------------------------------------------------------------------------
# 2. Define the 'cps_lib_ppsp' Static Library
# -----------------------------------------------------------------------------
# We gather all the .cpp sources from the various subfolders in cps/
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

    # Spraylist Priority Queue (Prefer .cpp over .c)
    ${CPS_ROOT}/include-spraylist/pqueue.cpp
)

add_library(cps_lib_ppsp STATIC ${CPS_SOURCES})

# -----------------------------------------------------------------------------
# 3. Apply Legacy Flags ONLY to 'cps_lib_ppsp'
# -----------------------------------------------------------------------------
# These flags are required for the legacy code to work but might be unsafe
# for the rest of your project. We limit them to 'cps_lib_ppsp'.
target_compile_options(cps_lib_ppsp PRIVATE
    -Wall
    -fno-strict-aliasing
    -D_REENTRANT
    -D_GNU_SOURCE
    -DLOCKFREE
    -DTLS
    -m64
)

# Pass the cache line size definition
target_compile_definitions(cps_lib_ppsp PRIVATE 
    CACHE_LINE_SIZE=${CACHE_LINE_SIZE} 
    INTEL
)

# -----------------------------------------------------------------------------
# 4. Setup Includes (Public so ppsp-cpu can find headers)
# -----------------------------------------------------------------------------
target_include_directories(cps_lib_ppsp PUBLIC
    ${CPS_ROOT}/common
    ${CPS_ROOT}/common/include
    ${CPS_ROOT}/common/atomic_ops
    ${CPS_ROOT}/include-linden
    ${CPS_ROOT}/include-spraylist
    ${CPS_ROOT}/common/gc
)

# Link standard libraries required by the legacy code
target_link_libraries(cps_lib_ppsp PRIVATE pthread rt)

# -----------------------------------------------------------------------------
# 5. Your Main Executable (ppsp-cpu)
# -----------------------------------------------------------------------------
add_executable(ppsp-cpu ppsp.cpp)

# Link your dependencies + the new cps_lib_ppsp
target_link_libraries(ppsp-cpu PRIVATE 
    Galois::shmem 
    lonestar 
    cps_lib_ppsp
)

add_dependencies(apps ppsp-cpu)
install(TARGETS ppsp-cpu DESTINATION "${CMAKE_INSTALL_BINDIR}" COMPONENT apps EXCLUDE_FROM_ALL)
