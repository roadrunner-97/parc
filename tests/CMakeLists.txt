include(FetchContent)
FetchContent_Declare(
    googletest
    URL https://github.com/google/googletest/archive/refs/tags/v1.15.2.tar.gz
    URL_HASH SHA256=7b42b4d6ed48810c5362c265a17faebe90dc2373c885e5216439d37927f02926
)
FetchContent_MakeAvailable(googletest)

add_executable(parc_util_tests
    test_bitstream.cpp
    test_block.cpp
    test_buf.cpp
    test_err.cpp
    test_frame.cpp
    test_gen.cpp
    test_golden.cpp
    test_huffman.cpp
    test_lz.cpp
    test_mt.cpp
    test_rng.cpp
    test_stats.cpp
    test_xxh64.cpp
)
target_link_libraries(parc_util_tests PRIVATE parc parc_gen GTest::gtest_main)
target_include_directories(parc_util_tests
    PRIVATE ${CMAKE_SOURCE_DIR}/src
)
target_compile_options(parc_util_tests PRIVATE -Wall -Wextra -Werror)
target_compile_definitions(parc_util_tests
    PRIVATE GOLDEN_DIR="${CMAKE_CURRENT_SOURCE_DIR}/golden")

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

# parc CLI end-to-end: file and pipe roundtrips, verify, and guaranteed-
# corrupt inputs (truncation and appended garbage) exiting nonzero.
add_test(NAME cli_parc_file_roundtrip
    COMMAND bash -c "\
        $<TARGET_FILE:parcgen> -s 12 -n 5M -o pz_in.bin text && \
        $<TARGET_FILE:parc_cli> c pz_in.bin -o pz.parc && \
        $<TARGET_FILE:parc_cli> d pz.parc -o pz_out.bin && \
        cmp pz_in.bin pz_out.bin && \
        $<TARGET_FILE:parc_cli> t pz.parc"
    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
add_test(NAME cli_parc_pipe_roundtrip
    COMMAND bash -c "\
        set -o pipefail && \
        $<TARGET_FILE:parcgen> -s 13 -n 3M -o pz_p.bin json-log && \
        $<TARGET_FILE:parc_cli> c -b 16 < pz_p.bin | \
        $<TARGET_FILE:parc_cli> d > pz_p.out && \
        cmp pz_p.bin pz_p.out"
    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
add_test(NAME cli_parc_mt_roundtrip
    COMMAND bash -c "\
        set -o pipefail && \
        $<TARGET_FILE:parcgen> -s 15 -n 9M -o pz_mt.bin json-log && \
        $<TARGET_FILE:parc_cli> c -b 16 -T 0 pz_mt.bin -o pz_mt.parc && \
        $<TARGET_FILE:parc_cli> c -b 16 pz_mt.bin | cmp pz_mt.parc - && \
        $<TARGET_FILE:parc_cli> d -T 4 pz_mt.parc -o pz_mt.out && \
        cmp pz_mt.bin pz_mt.out && \
        $<TARGET_FILE:parc_cli> t -T 0 pz_mt.parc"
    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
add_test(NAME cli_parc_rejects_corrupt
    COMMAND bash -c "\
        $<TARGET_FILE:parcgen> -s 14 -n 100K -o pz_c.bin text && \
        $<TARGET_FILE:parc_cli> c pz_c.bin -o pz_c.parc && \
        head -c -1 pz_c.parc > pz_trunc.parc && \
        ! $<TARGET_FILE:parc_cli> t pz_trunc.parc && \
        cat pz_c.parc <(printf x) > pz_tail.parc && \
        ! $<TARGET_FILE:parc_cli> t pz_tail.parc && \
        ! $<TARGET_FILE:parc_cli> d pz_trunc.parc -o /dev/null"
    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})

# Differential test against ent(1): semantic diff of every shared statistic
# over a synthetic suite of edge-case inputs. Registered only when ent and a
# Python interpreter are available.
find_program(ENT_EXECUTABLE ent)
find_package(Python3 COMPONENTS Interpreter)
if(ENT_EXECUTABLE AND Python3_Interpreter_FOUND)
    add_test(NAME cli_parcent_ent_diff
        COMMAND Python3::Interpreter
            ${CMAKE_SOURCE_DIR}/tools/parcent/ent-diff.py
            --parcent $<TARGET_FILE:parcent>
            --ent ${ENT_EXECUTABLE}
        WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
else()
    message(STATUS "ent or python3 not found; skipping cli_parcent_ent_diff")
endif()
