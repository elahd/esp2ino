# Development Workspaces

## Overview
1. These VS Code workspaces are constructed of dedicated folders and symlinks to simplify development for each platform. Each workspace has dedicated VS Code tasks for building and testing.
2. Separate workspaces are used to allow both ESP8266 and ESP32 variants of esp2ino to be built from the same core source files while each having their own build infrastructure.
3. Devcontainers exist for each workspace, but they dont work. As such, they're zipped to prevent VS Code from running them. As it turns out, devcontainers don't play well with symlinks.

## Usage
To use the backend workspaces, do the following (on MacOS -- not yet tested elsewhere):

1. Install both ESP8266_RTOS_SDK and ESP_IDF to their default install paths. That is:
   1. ESP8266_RTOS_SDK: `$HOME/esp/ESP8266_RTOS_SDK`
   2. ESP_IDF: `$HOME/esp/esp-idf`
2. Run the ESP_IDF install.sh script.
3. Download the ESP8266_RTOS_SDK's Xtensa toolchain to `$HOME/esp/xtensa-lx106-elf/bin`.
4. Add to your system PATH:
   1. `$HOME/esp/xtensa-lx106-elf/bin`
   2. `$HOME/.espressif/tools/xtensa-esp32-elf/esp-2020r3-8.4.0/xtensa-esp32-elf/bin`
5. Set the environment variable `IDF_PATH_ROOT` to `$HOME/esp`.

**To use the ESP8266 workspace**, first run `set_env.sh` from `esp2ino/workspaces/dev-resources`. Then, launch VS Code from the command line using code `esp2ino/workspaces/esp8266.code-workspace`.

**To use the ESP32 workspace**, first run `source $HOME/esp/esp-idf/export.sh`. Then, launch VS Code from the command line using code `esp2ino/workspaces/esp32.code-workspace`. **The ESP32 codebase is a work in progress. It's broken, will brick your device, and should not be used.**