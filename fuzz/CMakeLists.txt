if(NOT CMAKE_C_COMPILER_ID MATCHES "Clang")
    message(FATAL_ERROR "PARC_FUZZ needs clang (use the 'fuzz' preset)")
endif()

foreach(target fuzz_decode fuzz_roundtrip)
    add_executable(${target} ${target}.c)
    target_link_libraries(${target} PRIVATE parc)
    target_compile_options(${target} PRIVATE -Wall -Wextra -Wconversion
        -Werror -fsanitize=fuzzer)
    target_link_options(${target} PRIVATE -fsanitize=fuzzer)
endforeach()

# Short deterministic fuzz runs so `ctest --preset fuzz` exercises both
# targets; long campaigns are run manually (Phase 7).
add_test(NAME fuzz_decode_smoke
    COMMAND bash -c "\
        mkdir -p corpus_decode && \
        cp ${CMAKE_SOURCE_DIR}/tests/golden/*.parc corpus_decode/ && \
        $<TARGET_FILE:fuzz_decode> -runs=20000 -max_len=16384 corpus_decode"
    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
add_test(NAME fuzz_roundtrip_smoke
    COMMAND $<TARGET_FILE:fuzz_roundtrip> -runs=5000 -max_len=16384
    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
