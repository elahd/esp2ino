<div align="center">
<h1 style="border-bottom:0; margin:0;"><img style="margin-bottom:-0.3em" src="resources/logo/favicon-io/favicon-32x32.png"> esp2ino</h1>
<small><em>Formerly Wyze Plug Flasher</em></small>
</div>

***

Use esp2ino to load Arduino-based firmware like Tasmota, ESPurna, and ESPEasy on to ESP8266-based devices over-the-air.

***

## Purpose
The [ESP8266](https://www.espressif.com/en/products/socs/esp8266) microcontroller powers many consumer smart home products. These products come with manufacturer firmware that locks users into restrictive cloud ecosystems. esp2ino lets users install their own Arduino-based firmware onto these devices *over the air* — that is, without physically opening the device.
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

The trick is figuring out how to load esp2ino onto these devices. [HclX](https://github.com/HclX) figured it out for Wyze devices via [WyzeUpdater](https://github.com/HclX/WyzeUpdater). If you have a method for loading firmware................................

## Method
Manufacturer firmware for ESP8266-based devices is usually built on the [ESP8266 RTOS SDK](https://github.com/espressif/ESP8266_RTOS_SDK). This SDK is maintained by Espressif, the chip's manufacturer. ESP8266_RTOS_SDK's [bootloader](https://en.wikipedia.org/wiki/Bootloader) cannot boot Arduino-based firmware. There are two options for converting devices to run Arduino-based firmware:

1. **Serial Connection:** Physically open the device and load firmware over a serial connection. This is easy on some devices, but is destructive and time-consuming on others.
2. **esp2ino:** Trick the device into loading esp2ino. esp2ino runs once, then deletes itself. Once installed, esp2ino replaces the ESP8266_RTOS_SDK bootloader with Arduino's eboot bootloader. It then loads the Arduino-based firmware of your choice onto the device.

# ⚠️ Warnings & Liability Disclaimers
1. **This is a one-way conversion.** esp2ino has a backup function, but the only way to restore your device from the backup is over a serial connection. That is, you'd need to open your device and load the firmware over a serial connection.
2. **This is alpha software with alpha-level code quality, stability, and usability. It has not been thoroughly tested.** You're more likely to brick your device with this loader than you are with a more mature project like Tuya-Convert.
3. This project is not supported by or otherwise affiliated with Wyze or Espressif Systems. It's a hobby project maintained by an individual, built for personal use, and shared without any guarantees.
4. **Loading third party firmware may void your warranty.**
5. Loading third party firmware may brick your device 🧱, cause it to catch fire 🔥, or just cause unexpected behavior 🤪.

**This software is provided as-is with no guarantees as to quality, features, functionality, or safety. Use of this software is entirely at your own risk. You agree to not hold the author liable for any damage, loss of life, financial damages, loss of functionality, etc.**

***
# Misc

## esp2ino vs tuya-convert
esp2ino borrows concepts (and some code) from tuya-convert. For background, Tuya-convert has two basic parts:

1. **Firmware loader.** This tricks the IoT device into accepting third party firmware by emulating Tuya's cloud infrastructure.
2. **Intermediate firmware.** This firmware modifies the device's bootloader to accept Arduino-based firmwares.

esp2ino is different from tuya-convert in the following ways:

1. **Firmware loader.** esp2ino currently targets two Wyze products and uses the [WyzeUpdater](https://github.com/HclX/WyzeUpdater) script to push esp2ino's intermediate firmware onto devices. I'm looking for help identifying and building loaders for other devices, including from other manufacturers.
2. **Intermediate firmware.** Tuya-convert's intermediate firmware targets devices with firmware built on versions < 3.0 of the [ESP8266 RTOS SDK](https://github.com/espressif/ESP8266_RTOS_SDK). esp2ino targets devices with firmware build on [ESP8266 RTOS SDK](https://github.com/espressif/ESP8266_RTOS_SDK) versions >= v 3.0.

# Sources

This project borrows bits and pieces from:
1. [Tuya-Convert](https://github.com/ct-Open-Source/tuya-convert)
2. Espressif's [ESP8266_RTOS_SDK program samples](https://github.com/espressif/ESP8266_RTOS_SDK/tree/master/examples).