set(CMAKE_CXX_STANDARD 17)
set(CMAKE_C_FLAGS_RELEASE "${CMAKE_C_FLAGS_RELEASE} -O3")
set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} -O3")

# fetches google test code
include(FetchContent)
FetchContent_Declare(
        googletest
        GIT_REPOSITORY https://github.com/google/googletest.git
        GIT_TAG release-1.12.1
)

# For Windows: Prevent overriding the parent project's compiler/linker settings
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

enable_testing()

file (GLOB_RECURSE SOURCES LIST_DIRECTORIES false "*.cpp")
# qpid_validate.cpp is a standalone concurrency stress test with its own main()
# and needs C++20; it must not be swept into the gtest binary.
list(FILTER SOURCES EXCLUDE REGEX "qpid_validate\\.cpp$")
add_executable(all_test ${SOURCES})
target_link_libraries(all_test GTest::gtest_main ${Boost_LIBRARIES})

include(GoogleTest)
gtest_discover_tests(all_test)

# ---------------------------------------------------------------------------
# Standalone QPID correctness checker (not a gtest target).
#   cmake --build . --target qpid_validate
#   ./qpid_validate --help
# Or build and run the whole matrix directly with ./run_qpid_validate.sh
# ---------------------------------------------------------------------------
add_executable(qpid_validate EXCLUDE_FROM_ALL qpid_validate.cpp)
target_include_directories(qpid_validate PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/..")
target_compile_features(qpid_validate PRIVATE cxx_std_20)
target_compile_options(qpid_validate PRIVATE -O3 -march=native -fpermissive)
target_link_libraries(qpid_validate PRIVATE pthread)
