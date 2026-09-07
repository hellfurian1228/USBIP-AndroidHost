# USBIP-AndroidHost
<img width="256" height="256" alt="usbip" src="https://github.com/user-attachments/assets/4954a663-82e8-498c-a0f6-79545346c4ea" />

[![Status](https://img.shields.io/badge/Status-Release-green.svg?style=flat-square)](https://github.com/hellfurian1228/USBIP-AndroidHost)
[![Platform](https://img.shields.io/badge/Platform-Android-3DDC84.svg?style=flat-square&logo=android)](https://developer.android.com/about/dashboards)
[![Donate](https://img.shields.io/badge/Donate-PayPal-blue.svg?style=flat-square&logo=paypal)](https://www.paypal.com/donate/?hosted_button_id=P3P43EXYJWRLC)

## USBIP-AndroidHost
A streamlined Android implementation of the **USB/IP** protocol. This app transforms your Android device into a network-attached USB hub, allowing you to "tunnel" physical hardware—like sim equipment, game controllers, 3D printers/scanners, and storage devices—directly to a remote Windows or Linux machine.

## 🕹️ Why this exists
Most USB/IP solutions require complex Linux setups or rooted devices. This project aims to make the process accessible through a standard Android app, specifically tuned for low-latency peripherals like sim-racing gear.

## 📢 Project Update: The Future of USBIP-AndroidHost
This repository represents the final free, open-source version of USBIP-AndroidHost.

When I started this project, I stated that it would be a free solution, and I am honoring that promise. This existing open-source repository will always remain free and available to you here on GitHub.

I want to extend a massive thank you to everyone who has helped with testing, reporting bugs, and ensuring we could build a stable, functional, and truly free alternative to the other paid app out there (you know the one) with its predatory, hardware-locked licensing!

## 🚀 What's Next: Google Play Store Release
Moving forward, I will be continuing active development—adding new features, further optimizing the native C++ networking codebase, and refining the UI.

To help cover the costs of development tools, testing hardware, and the sheer amount of time I invest in maintaining this protocol, the next evolution of this app will be released as a fully approved, fixed-price app on the Google Play Store.

By purchasing the Play Store version, you will get:

Automatic Updates: Seamless background updates so you never have to manually install an APK again.

Dedicated Support: Priority troubleshooting for your specific hardware setups.

Exclusive New Features: Access to all future quality-of-life improvements, UI overhauls, and advanced networking features.

No Predatory Subscriptions: A flat, one-time fee for unlimited devices. No recurring charges, and no hardware-locked licenses.

Thank you for supporting this project and helping it grow from an experimental prototype into the low-latency networking tool it is today!

## ☕ Support the Project
If you find this tool useful and want to support continued development, I utilize subscription based software for code. Donations will go towards this. Thanks!

[**Donate via PayPal**](https://www.paypal.com/donate/?hosted_button_id=P3P43EXYJWRLC)

[**Donate via Ko-Fi**](https://ko-fi.com/mizukos)

## Discord https://discord.gg/R2nfbS4K2

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

*Note: This is an early beta. Use it, break it, and report issues to help improve stability.*
