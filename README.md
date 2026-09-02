# USBIP-AndroidHost
<img width="256" height="256" alt="usbip" src="https://github.com/user-attachments/assets/4954a663-82e8-498c-a0f6-79545346c4ea" />

[![Status](https://img.shields.io/badge/Status-Beta-orange.svg?style=flat-square)](https://github.com/hellfurian1228/USBIP-AndroidHost)
[![Platform](https://img.shields.io/badge/Platform-Android-3DDC84.svg?style=flat-square&logo=android)](https://developer.android.com/about/dashboards)
[![Donate](https://img.shields.io/badge/Donate-PayPal-blue.svg?style=flat-square&logo=paypal)](https://www.paypal.com/donate/?hosted_button_id=P3P43EXYJWRLC)

## ☕ Support the Project
If you find this tool useful and want to support continued development, I utilize subscription based software for code. Donations will go towards this. Thanks!

[**Donate via PayPal**](https://www.paypal.com/donate/?hosted_button_id=P3P43EXYJWRLC)

## Discord https://discord.gg/R2nfbS4K2

## USBIP-AndroidHost

A streamlined Android implementation of the **USB/IP** protocol. This app transforms your Android device into a network-attached USB hub, allowing you to "tunnel" physical hardware—like racing wheels, pedals, and game controllers—directly to a remote Windows or Linux machine.

## 🕹️ Why this exists
Most USB/IP solutions require complex Linux setups or rooted devices. This project aims to make the process accessible through a standard Android app, specifically tuned for low-latency peripherals like sim-racing gear.

## 📦 Installation & Usage

### 1. Installation
*   Download the latest `.apk` from the [Releases](https://github.com/hellfurian1228/USBIP-AndroidHost/releases) page.
*   Install it on your Android device (ensure "Install from Unknown Sources" is enabled).

### 2. Preparation (Host Side)
*   Connect your USB device (Wheel, Controller, etc.) to your Android device using a compatible **USB OTG** cable.
*   Launch the **USBIP-AndroidHost** app.
*   Grant USB permissions when prompted by the system.

### 3. Usage
*   Note the **Local IP Address** displayed at the top of the app.
*   Find your device in the list and tap **Connect**. 
*   Once it shows as "Connected," it is being exported to the network.

### 4. Client Side (Windows/Linux)
*   On your PC, launch your USB/IP client.
*   Input the Android device's IP and scan/attach the remote device.
*   The peripheral will now behave as if it were plugged directly into your PC.

## 🛠️ Tech Stack
*   **Kotlin:** Modern, lifecycle-aware UI and service management.
*   **C++ (JNI):** High-performance native server daemon for protocol handling.
*   **CMake:** Unified build system for native components.

## 📱 Tested & Working
*   Samsung Galaxy S20+ / S22+ Ultra / Z Fold 7
*   Google Pixel 10 Fold Pro
*   Essential PH-1
*   Samsung Tab A8

---

## 📜 Credits & Acknowledgements

This project is built upon the foundational work of the global open-source community:

*   **Takahiro Hirofuchi & the NAIST Research Team:** The original architects of the USB/IP protocol and researchers at the Nara Institute of Science and Technology.
*   **The Linux Kernel Community:** For maintaining and improving the core USB/IP drivers within the mainline kernel.
*   **cezanne (GitHub):** The creator of the `usbip-win` project, which successfully ported the Virtual Host Controller Interface (VHCI) to Windows.
*   **USBIP-Win2 Community:** For the ongoing development of modern Windows drivers and clients that this host is designed to communicate with.

*Note: This is an early beta. Use it, break it, and report issues to help improve stability.*
