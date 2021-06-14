#!/bin/bash

#https://stackoverflow.com/questions/370047/what-is-the-most-elegant-way-to-remove-a-path-from-the-path-variable-in-bash
path_append ()  { path_remove $1; export PATH="$PATH:$1"; }
path_prepend () { path_remove $1; export PATH="$1:$PATH"; }
path_remove ()  { export PATH=`echo -n $PATH | awk -v RS=: -v ORS=: '$0 != "'$1'"' | sed 's/:$//'`; }

export IDF_PATH_8266=$IDF_PATH_ROOT/ESP8266_RTOS_SDK
export TOOLCHAIN_PATH_8266=$IDF_PATH_ROOT/xtensa-lx106-elf

export IDF_PATH_32=$IDF_PATH_ROOT/esp-idf
export TOOLCHAIN_PATH_32=$IDF_PATH_ROOT/xtensa-esp32-elf

export IDF_PATH=$IDF_PATH_8266

path_append $IDF_PATH_8266/tools
path_append $TOOLCHAIN_PATH_8266/bin

path_remove $IDF_PATH_32/tools
path_remove $TOOLCHAIN_PATH_32/bin

path_remove $IDF_PATH_ROOT/esp-idf/components/esptool_py/esptool
path_remove $IDF_PATH_ROOT/esp-idf/components/espcoredump
path_remove $IDF_PATH_ROOT/esp-idf/components/partition_table
path_remove $IDF_PATH_ROOT/esp-idf/components/app_update
path_remove $IDF_PATH_ROOT/esp-idf/tools

# /Users/Elahd/.espressif/tools/openocd-esp32/v0.10.0-esp32-20210401/openocd-esp32/bin