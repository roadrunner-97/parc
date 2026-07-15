cmake_minimum_required(VERSION 3.25)
project(parc VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_package(Threads REQUIRED)

add_library(parc STATIC
    src/codec/block.c
    src/codec/frame.c
    src/codec/frame_mt.c
    src/codec/fse.c
    src/codec/huffman.c
    src/codec/lz.c
    src/codec/mt.c
    src/stat/stats.c
    src/util/bitstream.c
    src/util/buf.c
    src/util/err.c
    src/util/prof.c
    src/util/rng.c
    src/util/xxh64.c
)
target_include_directories(parc
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src
)
target_compile_options(parc PRIVATE -Wall -Wextra -Wconversion -Werror)
target_link_libraries(parc PUBLIC m Threads::Threads)

# Per-stage pipeline timing (src/util/prof.*). Off by default and zero-cost
# when off; when on, the single-thread compress/decompress path prints a stage
# breakdown to stderr. PUBLIC so the definition reaches every TU that includes
# util/prof.h. See the `prof` preset for a ready-made profiling build.
option(PARC_PROF "Instrument encode/decode with per-stage timers" OFF)
if(PARC_PROF)
    target_compile_definitions(parc PUBLIC PARC_PROF)
endif()

add_subdirectory(tools/parc)
add_subdirectory(tools/parcgen)
add_subdirectory(tools/parcent)

option(PARC_BENCH "Build corpus benchmarks (needs zlib, lz4, zstd)" OFF)
if(PARC_BENCH)
    add_subdirectory(bench)
endif()

include(CTest)
if(BUILD_TESTING)
    add_subdirectory(tests)
endif()

# after include(CTest) so the fuzz smoke runs register with CTest
option(PARC_FUZZ "Build libFuzzer targets (needs clang)" OFF)
if(PARC_FUZZ)
    add_subdirectory(fuzz)
endif()
