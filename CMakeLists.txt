cmake_minimum_required(VERSION 3.25)
project(parc VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

add_library(parc STATIC
    src/stat/stats.c
    src/util/bitstream.c
    src/util/buf.c
    src/util/err.c
    src/util/rng.c
    src/util/xxh64.c
)
target_include_directories(parc
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src
)
target_compile_options(parc PRIVATE -Wall -Wextra -Wconversion -Werror)
target_link_libraries(parc PUBLIC m)

add_subdirectory(tools/parcgen)

include(CTest)
if(BUILD_TESTING)
    add_subdirectory(tests)
endif()
