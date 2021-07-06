<div align="center">
<img src="https://github.com/elahd/esp2ino/blob/main/resources/logo/favicon-io/favicon-32x32.png?raw=true">
<h1>esp2ino</h1>
 <sub><em>Formerly Wyze Plug Flasher</em></sub>
</div>

***

Free your ESP8266-based smart home devices from vendor lock-in. esp2ino loads Arduino-based third-party firmware (Tasmota, ESPurna, ESPEasy, etc.) on to ESP8266-based devices _over-the-air_.

***

## Purpose
The [ESP8266](https://www.espressif.com/en/products/socs/esp8266) is a cheap and ubiquitous microcontroller that powers many consumer smart home devices. These products are typically locked into restrictive manufacturer cloud ecosystems. esp2ino lets users install their own Arduino-based firmware onto these devices *over the air* — that is, without physically opening the device.
## Supported Devices
### Out Of The Box
1. [**Wyze Plug**](https://wyze.com/wyze-plug.html) (model WLPP1)
2. [**Wyze Bulb**](https://wyze.com/wyze-bulb.html) (model WLPA19)

### Theoretical
 Although untested, esp2ino should support any ESP8266-based device that meets the below criteria:
1. Uses an ESP8266 microchip.
2. Uses [version 3](https://docs.espressif.com/projects/esp8266-rtos-sdk/en/latest/api-guides/fota-from-old-new.html) of the [ESP8266 RTOS SDK](https://github.com/espressif/ESP8266_RTOS_SDK).
3. Uses ESP8266 RTOS SDK's [built-in FOTA libraries](https://github.com/espressif/ESP8266_RTOS_SDK/tree/master/examples/system/ota).
4. Uses ESP8266 RTOS SDK's standard ["Two OTA" partition map](https://docs.espressif.com/projects/esp8266-rtos-sdk/en/latest/api-guides/partition-tables.html).

The key to using esp2ino is figuring out how to load it onto these devices. [HclX](https://github.com/HclX) figured it out for Wyze devices via [WyzeUpdater](https://github.com/HclX/WyzeUpdater). Please reach out or share a loader if you have a method for loading firmware onto other ESP8266-based devices!

## Method
Manufacturer firmware for ESP8266-based devices is usually built on the [ESP8266 RTOS SDK](https://github.com/espressif/ESP8266_RTOS_SDK). This SDK is maintained by Espressif, the chip's manufacturer. ESP8266_RTOS_SDK's [bootloader](https://en.wikipedia.org/wiki/Bootloader) cannot boot Arduino-based firmware. There are two options for converting devices to run Arduino-based firmware:

1. **Serial Connection:** Physically open the device and load firmware over a serial connection. This is easy on some devices, but is destructive and time-consuming on others.
2. **esp2ino:** Trick the device into loading esp2ino. Once installed, esp2ino replaces the ESP8266_RTOS_SDK bootloader with Arduino's eboot bootloader. It then loads the Arduino-based firmware of your choice onto the device. Finally, esp2ino runs once, then deletes itself.

# ⚠️ Warnings & Liability Disclaimers
1. **This is a one-way conversion.** esp2ino has a backup function, but the only way to restore your device from the backup is over a serial connection. That is, you'd need to open your device and solder wires to its PCB.
2. **This is alpha software with alpha-level code quality and stability. It has not been thoroughly tested.** You're more likely to brick your device with this loader than you are with a more mature project like Tuya-Convert.
3. This project is not supported by or otherwise affiliated with Wyze or Espressif Systems. It's a hobby project maintained by an individual, built for personal use, and shared without any guarantees.
4. **Loading third party firmware may void your warranty.**
5. Loading third party firmware may brick your device 🧱, cause it to catch fire 🔥, or just cause unexpected behavior 🤪. Liability for this falls on you!

**This software is provided as-is with no guarantees as to quality, features, functionality, or safety. Use of this software is entirely at your own risk. You agree to not hold the author liable for damage, loss of life, financial damages, loss of functionality, etc.**

***

# Install Instructions

See install page in the [wiki](https://github.com/elahd/esp2ino/wiki).

***

# Contributing

See README.md in [workspaces/](https://github.com/elahd/esp2ino/tree/main/workspaces), or just <a href="https://www.buymeacoffee.com/elahd" target="_blank">buy me a coffee</a>.

***

# esp2ino vs tuya-convert
esp2ino borrows concepts (and some code) from tuya-convert. For background, Tuya-convert has two basic parts:

1. **Firmware loader.** This tricks the IoT device into accepting third party firmware by emulating Tuya's cloud infrastructure.
2. **Intermediate firmware.** This firmware modifies the device's bootloader to accept Arduino-based firmwares.

esp2ino is different from tuya-convert in the following ways:

1. **Firmware loader.** esp2ino currently targets two Wyze products and uses the [WyzeUpdater](https://github.com/HclX/WyzeUpdater) script to push esp2ino's intermediate firmware onto these devices. Tuya-convert targets devices manufactured by [Tuya Smart](https://www.tuya.com/) and loads itself onto devices using Tuya-convert's own loader. (Tuya devices are sold under white label by [various manufacturers](https://www.google.com/search?q=site%3Atemplates.blakadder.com+tuya-convert).)
2. **Intermediate firmware.** Tuya-convert's intermediate firmware targets devices with firmware built on versions < 3.0 of the [ESP8266 RTOS SDK](https://github.com/espressif/ESP8266_RTOS_SDK). esp2ino targets devices with firmware built on [ESP8266 RTOS SDK](https://github.com/espressif/ESP8266_RTOS_SDK) versions >= v 3.0.

# Sources

esp2ino builds on:
1. [Tuya-Convert](https://github.com/ct-Open-Source/tuya-convert)
2. Espressif's [ESP8266_RTOS_SDK program samples](https://github.com/espressif/ESP8266_RTOS_SDK/tree/master/examples).
3. [vtrust's](https://www.vtrust.de) [esp8266-ota-flash-convert](https://github.com/vtrust-de/esp8266-ota-flash-convert)