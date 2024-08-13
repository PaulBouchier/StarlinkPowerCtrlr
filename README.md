# StarlinkPowerCtrlr

The Starlink Power Controller code runs two devices for controlling power
to the Starlink antenna, and for displaying battery status. The code runs on two M5StickCPlus2's that are on the
RV wifi network: the Starlink FOB, and the Starlink Battery Monitor.
See the design documentation for a description of the system.

## Design Documentation

System design documentation and requirements are at:
https://sites.google.com/site/paulbouchier/home/projects/starlink

## Modes

The Starlink Power Controller code can run in three modes:
1. Battery Monitor - the M5Stick installed in the battery box turns power to the antenna on/off, and
reports battery status to the FOB
2. Local FOB - the M5Stick FOB displays battery status and allows remotely turning antenna power on/off
3. Remote FOB - the M5Stick FOB connects to a network in the RV which is remote from the battery-box
network and provides the same capabilities as a local FOB

## Wifi passwords

The software has a menuing system which lets you select an SSID to connect to, and allows setting the
wifi password in the manner of setting date/time on a watch.

## Build

### Prerequisites

You must have the M5Stack board support installed in your Arduino environment - follow
these instructions: https://docs.m5stack.com/en/arduino/arduino_ide

Install required libraries using these instructions: https://roboticsbackend.com/install-arduino-library-from-github/

Required libraries:
- ESP32Ping library from  https://github.com/marian-craciunescu/ESP32Ping
- M5StickCPlus from https://github.com/m5stack/M5StickC-Plus
- M5StickCPlus2 from https://github.com/m5stack/M5StickCPlus2

