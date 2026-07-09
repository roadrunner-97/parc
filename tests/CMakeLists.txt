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
    test_rng.cpp
    test_xxh64.cpp
)
target_link_libraries(parc_util_tests PRIVATE parc GTest::gtest_main)
target_include_directories(parc_util_tests
    PRIVATE ${CMAKE_SOURCE_DIR}/src
)
target_compile_options(parc_util_tests PRIVATE -Wall -Wextra -Werror)

include(GoogleTest)
gtest_discover_tests(parc_util_tests)
