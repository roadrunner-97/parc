add_library(parc_gen STATIC gen.c)
target_link_libraries(parc_gen PUBLIC parc)
target_include_directories(parc_gen
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}
    PRIVATE ${CMAKE_SOURCE_DIR}/src
)
target_compile_options(parc_gen PRIVATE -Wall -Wextra -Wconversion -Werror)
