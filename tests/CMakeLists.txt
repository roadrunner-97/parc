include(FetchContent)
FetchContent_Declare(
    googletest
    URL https://github.com/google/googletest/archive/refs/tags/v1.15.2.tar.gz
    URL_HASH SHA256=7b42b4d6ed48810c5362c265a17faebe90dc2373c885e5216439d37927f02926
)
FetchContent_MakeAvailable(googletest)

add_executable(parc_util_tests
    test_bitstream.cpp
    test_buf.cpp
    test_err.cpp
    test_gen.cpp
    test_rng.cpp
    test_stats.cpp
    test_xxh64.cpp
)
target_link_libraries(parc_util_tests PRIVATE parc parc_gen GTest::gtest_main)
target_include_directories(parc_util_tests
    PRIVATE ${CMAKE_SOURCE_DIR}/src
)
target_compile_options(parc_util_tests PRIVATE -Wall -Wextra -Werror)

include(GoogleTest)
gtest_discover_tests(parc_util_tests)

# CLI smoke tests. The thread-invariance test is the one that exercises
# parcent's worker threads under TSan.
add_test(NAME cli_parcgen_deterministic
    COMMAND bash -c "\
        $<TARGET_FILE:parcgen> -s 5 -n 1M -o cli_a.bin text && \
        $<TARGET_FILE:parcgen> -s 5 -n 1M -o cli_b.bin text && \
        cmp cli_a.bin cli_b.bin"
    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
add_test(NAME cli_parcent_thread_invariance
    COMMAND bash -c "\
        $<TARGET_FILE:parcgen> -s 6 -n 8M -o cli_t.bin json-log && \
        $<TARGET_FILE:parcent> -c -t 1 cli_t.bin > cli_t1.csv && \
        $<TARGET_FILE:parcent> -c -t 8 cli_t.bin > cli_t8.csv && \
        diff cli_t1.csv cli_t8.csv"
    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
add_test(NAME cli_parcent_stdin_matches_file
    COMMAND bash -c "\
        $<TARGET_FILE:parcgen> -s 8 -n 3M -o cli_s.bin records 64 && \
        $<TARGET_FILE:parcent> -c cli_s.bin | cut -d, -f2-10 > cli_f.csv && \
        $<TARGET_FILE:parcent> -c < cli_s.bin | cut -d, -f2-10 > cli_p.csv && \
        diff cli_f.csv cli_p.csv"
    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
