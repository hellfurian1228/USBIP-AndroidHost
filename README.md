# USBIP-AndroidHost

[![Status](https://img.shields.io/badge/Status-Beta-orange.svg?style=flat-square)](https://github.com/hellfurian1228/USBIP-AndroidHost)
[![Platform](https://img.shields.io/badge/Platform-Android-3DDC84.svg?style=flat-square&logo=android)](https://developer.android.com/about/dashboards)

A streamlined Android implementation of the **USB/IP** protocol. This app transforms your Android device into a network-attached USB hub, allowing you to "tunnel" physical hardware—like racing wheels, pedals, and game controllers—directly to a remote Windows or Linux machine.

## 🕹️ Why this exists
Most USB/IP solutions require complex Linux setups or rooted devices. This project aims to make the process accessible through a standard Android app, specifically tuned for low-latency peripherals like sim-racing gear.

## 🚀 Quick Start

### 1. Prepare the Host (Android)
*   Connect your USB device via an **OTG cable**.
*   Launch the app and grant USB permissions when prompted.
*   Note the **IP Address** displayed at the top of the screen.
*   Tap **Connect** on the device you wish to share.

### 2. Connect the Client (Windows/Linux)
*   On your PC, use a USB/IP client (like the companion Windows Client) to attach to the Android IP.
*   The device will appear on your PC as if it were plugged in locally.

## 🛠️ Tech Stack
*   **Kotlin:** Modern, lifecycle-aware UI and service management.
*   **C++ (JNI):** High-performance native server daemon for protocol handling.
*   **CMake:** Unified build system for native components.

## 📱 Tested & Working
*   Samsung Galaxy S20+ / S22+ Ultra / Z Fold 7
*   Google Pixel 10 Fold Pro
*   Essential PH-1
*   Samsung Tab A8

## 📂 Project Structure
*   `/app` - Android source and UI logic.
*   `/app/src/main/cpp` - Native USB/IP server implementation.
*   `/.github/workflows` - Automated build scripts for APK releases.

---

### Acknowledgements
Built on the groundwork of the open-source USB/IP ecosystem, including research by **Takahiro Hirofuchi** and the `usbip-win` port by **cezanne**.

*Note: This is an early beta. Use it, break it, and report issues to help improve stability.*
