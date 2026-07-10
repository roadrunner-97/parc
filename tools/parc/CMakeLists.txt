add_executable(parc_cli main.c)
set_target_properties(parc_cli PROPERTIES OUTPUT_NAME parc)
target_link_libraries(parc_cli PRIVATE parc)
target_compile_options(parc_cli PRIVATE -Wall -Wextra -Wconversion -Werror)
