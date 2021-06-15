# Development Workspaces

## Overview
1. These VS Code workspaces are constructed of dedicated folders and symlinks to simplify development for each platform. Each workspace has dedicated VS Code tasks for building and testing.
2. There are separate workspaces for both the backend and frontend to allow both to be built with their own build infrastructure.
3. Devcontainers exist for each workspace, but they dont work. As such, they're zipped to prevent VS Code from running them. As it turns out, devcontainers don't play well with symlinks.
4. Folders and files with references to the ESP32 are vestigal. If I end up making an ESP32 version of esp2ino, it'll use a completely separate codebase.

## Usage
To use the backend workspaces, do the following (on MacOS -- not yet tested elsewhere):

1. Install the ESP8266_RTOS_SDK its default install path, `$HOME/esp/ESP8266_RTOS_SDK`.
2. Download the ESP8266_RTOS_SDK's Xtensa toolchain to `$HOME/esp/xtensa-lx106-elf/bin`.
3. Add to your system PATH:
   1. `$HOME/esp/xtensa-lx106-elf/bin`
4. Set the environment variable `IDF_PATH_ROOT` to `$HOME/esp`.

**To use the ESP8266 workspace**, first run `set_env.sh` from `esp2ino/main/source/firmware/backend/esp8266/dev-resources`. Then, launch VS Code from the command line using code `esp2ino/workspaces/esp8266.code-workspace`.