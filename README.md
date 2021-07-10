![GitHub release (latest by date)](https://img.shields.io/github/v/release/elahd/esp2ino) ![GitHub all releases](https://img.shields.io/github/downloads/elahd/esp2ino/total) ![GitHub](https://img.shields.io/github/license/elahd/esp2ino) [![Buy Me A Coffee](https://img.shields.io/badge/%20-Buy%20Me%20A%20Coffee-grey?logo=buy-me-a-coffee)](https://www.buymeacoffee.com/elahd)

***

<div align="center">
<img src="https://github.com/elahd/esp2ino/blob/main/resources/logo/favicon-io/favicon-32x32.png?raw=true">
<h1>esp2ino</h1>
 <sub><em>Formerly Wyze Plug Flasher</em></sub>
</div>

***

Free your ESP8266-based smart home devices from vendor lock-in. esp2ino loads Arduino-based third-party firmware (Tasmota, ESPurna, ESPEasy, etc.) on to ESP8266-based devices _over-the-air_.

***

## Purpose

The [ESP8266](https://www.espressif.com/en/products/socs/esp8266) is a cheap and ubiquitous microcontroller that powers many consumer smart home devices, most of which are agents of [vendor lock in](https://en.wikipedia.org/wiki/Vendor_lock-in). esp2ino lets you install your own Arduino-based firmware onto ESP8266-based devices *over the air* — that is, without physically opening the device.

## Supported Devices

1. [__Wyze Plug__](https://wyze.com/wyze-plug.html) (model WLPP1)
2. [__Wyze Bulb__](https://wyze.com/wyze-bulb.html) (model WLPA19)

Theoretically, esp2ino can convert most new ESP8266 based devices. See [Contibuting](https://github.com/elahd/esp2ino/wiki/Contributing) in the wiki for more information.

## How it Works

Manufacturer firmware for ESP8266-based devices is usually built on the [ESP8266 RTOS SDK](https://github.com/espressif/ESP8266_RTOS_SDK). This SDK is maintained by Espressif, the chip's manufacturer. ESP8266_RTOS_SDK's [bootloader](https://en.wikipedia.org/wiki/Bootloader) cannot boot Arduino-based firmware. In order to boot Arduino-based firmware, you must replace the ESP8266_RTOS_SDK bootloader. There are two options for doing this:

1. __Serial Connection:__ Physically open the device and load firmware over a serial connection while the device is offline. This is easy on some devices, but is destructive and time-consuming on others.
2. __esp2ino:__ Force the device to load esp2ino. Once installed, esp2ino replaces the ESP8266_RTOS_SDK bootloader with Arduino's eboot bootloader _while the device is running_. It then loads the Arduino-based firmware of your choice onto the device. After performing this critical task, esp2ino deletes itself.

# ⚠️ Warnings & Liability Disclaimers

1. __This is a one-way conversion.__ esp2ino has a backup function, but the only way to restore your device from the backup is over a serial connection. That is, you'd need to open your device and solder wires to its PCB.
2. __This is alpha software with alpha-level code quality and stability. It has not been thoroughly tested.__ You're more likely to brick your device with this loader than you are with a more mature project like Tuya-Convert.
3. This project is not supported by or otherwise affiliated with Wyze or Espressif Systems. It's a hobby project maintained by an individual, built for personal use, and shared without any guarantees.
4. __Loading third party firmware may void your warranty.__
5. Loading third party firmware may brick your device 🧱, cause it to catch fire 🔥, or just cause unexpected behavior 🤪. Liability for this falls on you!

__This software is provided as-is with no guarantees as to quality, features, functionality, or safety. Use of this software is entirely at your own risk. You agree to not hold the author liable for damage, loss of life, financial damages, loss of functionality, etc.__

***

# Install Instructions

See install page in the [wiki](https://github.com/elahd/esp2ino/wiki).

***

# Contributing

See [Contributing]([wiki](https://github.com/elahd/esp2ino/wiki/Contributing)) page in the wiki or just <a href="https://www.buymeacoffee.com/elahd" target="_blank">buy me a coffee</a>.

***

# esp2ino vs tuya-convert

esp2ino borrows concepts from tuya-convert. For background, tuya-convert has two basic parts:

1. __Firmware loader.__ This tricks the IoT device into accepting third party firmware by emulating Tuya's cloud infrastructure.
2. __Intermediate firmware.__ This firmware modifies the device's bootloader to accept Arduino-based firmwares.

esp2ino is different from tuya-convert in the following ways:

1. __Firmware loader.__ esp2ino currently targets two Wyze products and uses the [WyzeUpdater](https://github.com/HclX/WyzeUpdater) script to push esp2ino's intermediate firmware onto these devices. Tuya-convert targets devices manufactured by [Tuya Smart](https://www.tuya.com/) and loads itself onto devices using Tuya-convert's own loader. (Tuya devices are sold under white label by [various manufacturers](https://www.google.com/search?q=site%3Atemplates.blakadder.com+tuya-convert).)
2. __Intermediate firmware.__ Tuya-convert's intermediate firmware targets devices with firmware built on versions < 3.0 of the [ESP8266 RTOS SDK](https://github.com/espressif/ESP8266_RTOS_SDK). esp2ino targets devices with firmware built on [ESP8266 RTOS SDK](https://github.com/espressif/ESP8266_RTOS_SDK) versions >= v 3.0.

# Sources

esp2ino builds on:

1. [Tuya-Convert](https://github.com/ct-Open-Source/tuya-convert)
2. Espressif's [ESP8266_RTOS_SDK program samples](https://github.com/espressif/ESP8266_RTOS_SDK/tree/master/examples).
3. [vtrust's](https://www.vtrust.de) [esp8266-ota-flash-convert](https://github.com/vtrust-de/esp8266-ota-flash-convert)
