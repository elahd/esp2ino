idf_component_register(
    SRCS "esp2ino_main.c" "eboot_command.c" "esp2ino_webDebug.c" "esp2ino_wifi.c" "esp2ino_server.c" "esp2ino_safemode.c" "../static/index.html.gz.c" "../static/eboot_bin.c"
    INCLUDE_DIRS "." "../static" "../components"
)


# set_source_files_properties(mysrc.c
# PROPERTIES COMPILE_FLAGS
# -Wno-unused-variable
# )