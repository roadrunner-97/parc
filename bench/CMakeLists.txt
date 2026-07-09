include(FetchContent)

set(BENCHMARK_ENABLE_TESTING OFF)
set(BENCHMARK_ENABLE_GTEST_TESTS OFF)
set(BENCHMARK_ENABLE_INSTALL OFF)
FetchContent_Declare(
    googlebenchmark
    URL https://github.com/google/benchmark/archive/refs/tags/v1.9.1.tar.gz
    URL_HASH SHA256=32131c08ee31eeff2c8968d7e874f3cb648034377dfc32a4c377fa8796d84981
)
FetchContent_MakeAvailable(googlebenchmark)

find_package(ZLIB REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(LZ4 REQUIRED IMPORTED_TARGET liblz4)
pkg_check_modules(ZSTD REQUIRED IMPORTED_TARGET libzstd)

add_executable(parc_bench bench_corpus.cpp)
target_link_libraries(parc_bench PRIVATE
    parc benchmark::benchmark ZLIB::ZLIB PkgConfig::LZ4 PkgConfig::ZSTD)
target_include_directories(parc_bench PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_compile_definitions(parc_bench
    PRIVATE PARC_CORPUS_DIR="${CMAKE_SOURCE_DIR}/corpus")
target_compile_options(parc_bench PRIVATE -Wall -Wextra -Werror)
