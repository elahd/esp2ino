idf_component_register(
    SRCS "esp2ino_main.c" "eboot_command.c"
    INCLUDE_DIRS "../static"
)


# set_source_files_properties(mysrc.c
# PROPERTIES COMPILE_FLAGS
# -Wno-unused-variable
# )