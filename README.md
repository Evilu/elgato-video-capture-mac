# Elgato Video Capture for macOS

A macOS driver for the Elgato Video Capture USB device (the analogue capture dongle sold for digitising VHS), written by studying Elgato's Windows driver.

The Windows driver is the specification: it is unpacked and decompiled to learn how the device is initialised and how video and audio come off the USB bus, and a macOS driver is then written to do the same. No Elgato code is carried over.

**Status: nothing built yet.** The repository holds this plan. The installer has been identified but not unpacked.

## What is known so far

Measured from the installer file itself (`Video_Capture_Driver_Installer.exe`, 3,220,128 bytes, SHA-256 `3ba345bb3b9d21e264d31f1e78333c78ef6ce856c6615f28dffe7fb906b463f7`):

- It is a 32-bit Windows executable made with Nullsoft Install System 2.45.1 (Unicode), signed by Elgato Systems.
- It asks for administrator rights and declares support for Windows Vista and Windows 7.
- Its contents are compressed: no driver file names, USB IDs or chipset names are readable until it is unpacked.

Not known yet, and the first things to find out:

- Which files it installs (`.sys`, `.inf`, firmware), and for which architectures.
- The device's USB vendor and product IDs, and whether there is more than one hardware revision.
- The capture chipset, and whether firmware is uploaded to the device at start-up.
- Whether the device can be driven from user space on macOS, or needs a system extension.

## Plan

1. **Unpack** the installer into `analysis/` and list what it installs.
2. **Read the `.inf`** for the USB IDs, the driver's service name and any firmware files.
3. **Decompile the driver** with Ghidra: the device start-up sequence, the register writes that select input and video standard, and the layout of the video and audio streams.
4. **Capture the real thing**: USB traces of the device under Windows, to check the reading of the code against what is sent on the wire.
5. **Prove it from user space on macOS** with a small command-line tool that opens the device, starts it and saves raw frames.
6. **Make it a capture device** other applications can see.

Steps 5 and 6 depend on what steps 1 to 4 find; they will be planned in detail then.

## The installer is not in this repository

This repository contains no Elgato files: no installer, no driver binaries, no firmware, no decompiler output. `.gitignore` keeps them out. To work on the project you supply your own copy of the installer, downloaded from Elgato, and everything derived from it stays in `analysis/` on your own machine.

## Layout

| Path | Contents |
|---|---|
| `README.md` | This file |
| `analysis/` (ignored) | The unpacked installer, Ghidra projects and exports, USB traces |
| `docs/` (to come) | What was learned about the device and the driver |
| `tools/` (to come) | Scripts for unpacking, decompiling and tracing |
| `src/` (to come) | The macOS driver |

## Rights

Elgato, Elgato Video Capture and the Windows driver belong to their owners (Elgato, a Corsair brand). This is an independent interoperability project with no affiliation with or endorsement by them. The driver is studied only to make hardware its owner has bought work on another operating system.
