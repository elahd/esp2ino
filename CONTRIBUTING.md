# Supporting New Devices

 Although untested, esp2ino should support any ESP8266-based device that meets the below criteria:

1. Uses an ESP8266 microchip.
2. Uses [version 3](https://docs.espressif.com/projects/esp8266-rtos-sdk/en/latest/api-guides/fota-from-old-new.html) of the [ESP8266 RTOS SDK](https://github.com/espressif/ESP8266_RTOS_SDK).
3. Uses ESP8266 RTOS SDK's [built-in FOTA libraries](https://github.com/espressif/ESP8266_RTOS_SDK/tree/master/examples/system/ota).
4. Uses ESP8266 RTOS SDK's standard ["Two OTA" partition map](https://docs.espressif.com/projects/esp8266-rtos-sdk/en/latest/api-guides/partition-tables.html).

The key to using esp2ino is figuring out how to load it onto these devices. [HclX](https://github.com/HclX) figured it out for Wyze devices via [WyzeUpdater](https://github.com/HclX/WyzeUpdater). Please reach out or share a loader if you have a method for loading firmware onto other ESP8266-based devices!

# Writing Code

esp2ino is developed in three VS Code folder-workspaces. You can access these workspaces (and their suite of build/dev tools) by opening one of the following folders in VS Code:

1. __/source/firmware/backend/esp8266__ for developing the firmware.
2. __/source/firmware/frontend__ for developing the firmware's web frontend.
3. __/ (root)__ for everything else.

## Overview

1. Each workspace has dedicated VS Code tasks for building and testing.
2. Ignore the VS Code devcontainers. They don't work well.
3. Folders and files with references to the ESP32 are vestigal. If I end up making an ESP32 version of esp2ino, it'll use a completely separate codebase.

## Setting Up Your Dev Environment

To use the backend workspace, do the following (on MacOS -- not yet tested elsewhere):

1. Install the ESP8266_RTOS_SDK its default install path, `$HOME/esp/ESP8266_RTOS_SDK`.
2. Download the ESP8266_RTOS_SDK's Xtensa toolchain to `$HOME/esp/xtensa-lx106-elf/bin`.
3. Add to your system PATH:
   1. `$HOME/esp/xtensa-lx106-elf/bin`
4. Set the environment variable `IDF_PATH_ROOT` to `$HOME/esp`.

**To use the ESP8266 workspace**, first run `source set_env.sh` from `esp2ino/main/source/firmware/backend/esp8266/dev-resources`. Then, launch VS Code from the command line by using `code .` from within the workspace folder.
