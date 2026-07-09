find_package(Threads REQUIRED)

add_executable(parcent main.c)
target_link_libraries(parcent PRIVATE parc Threads::Threads)
target_include_directories(parcent PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_compile_options(parcent PRIVATE -Wall -Wextra -Wconversion -Werror)
