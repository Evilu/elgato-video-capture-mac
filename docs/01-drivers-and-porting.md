# 1. Drivers, video capture drivers, and what "porting" means here

This chapter is background: how a capture driver is built on Windows, what the equivalent pieces are on macOS, and why the route from one to the other is a rewrite guided by reverse engineering rather than a recompile.

## 1.1 What a USB capture driver actually does

A driver for a device like this one has three jobs, and it helps to keep them apart because they port very differently.

| Job | What it involves | How OS-specific is it? |
|---|---|---|
| **Device protocol** | The exact USB requests that power the chip up, program its registers, select an input and video standard, and start the data endpoints. The format of the bytes that come back. | Not at all. It is a property of the hardware. The same bytes must go on the wire from any OS. |
| **Transport plumbing** | Opening the device, claiming interfaces, choosing alternate settings, queueing isochronous transfers, handling unplug. | Entirely. Every OS has its own USB API, but they all express the same USB concepts. |
| **Media framework glue** | Presenting the result as "a camera" or "a capture source": format negotiation, buffers, timestamps, property pages. | Entirely, and it is usually the bulk of the code. |

In the Windows driver studied here, roughly a third of the functions are device protocol; the rest is Kernel Streaming glue that has no meaning outside Windows.

## 1.2 The Windows side: WDM, Kernel Streaming, AVStream

`elvidcap.sys` is a **WDM kernel driver** written against **AVStream** (`ks.sys`), Microsoft's kernel-mode streaming framework. Its import table shows the whole stack:

- `ks.sys` (`KsInitializeDriver`, `KsCreateFilterFactory`, `KsStreamPointer*`, `KsPinGetReferenceClockInterface`): the driver describes itself as *filters* with *pins*. A capture pin produces video samples, another produces audio. User-mode DirectShow sees these through `ksproxy.ax` as a capture filter, which is why OBS on Windows can use the device with no Elgato software.
- `usbd.sys` (`USBD_ParseConfigurationDescriptorEx`, `USBD_CreateConfigurationRequestEx`) plus `IoBuildDeviceIoControlRequest` with `IOCTL_INTERNAL_USB_SUBMIT_URB` (0x220003): USB I/O is done by filling in **URB** structures and sending them down the device stack as IRPs.
- `bdasup.sys`: the Broadcast Driver Architecture support library, for digital TV tuners. The Conexant reference code this driver descends from supports TV tuner boards; the Elgato board has no tuner and the INF sets `Enable_BDA = 0`.
- `ntoskrnl.exe` registry calls (`ZwOpenKey`, `ZwQueryValueKey`): board configuration is read from registry values that the INF file writes at install time. The INF is therefore part of the driver's configuration, not just an installer script.

The companion files are user-mode DirectShow components: `CxPolaris.ax` (a property page), `cpnotify.ax` (copy-protection notification filter), `y8cnvt.ax` (a Y8 colour converter) and `cxtvrate.dll` (TV ratings). None of them touch the device.

## 1.3 The macOS side: four possible places for a driver

| Mechanism | Runs in | Status on current macOS | Fit for this device |
|---|---|---|---|
| **Kernel extension (IOKit kext)** | Kernel | Deprecated. Needs reduced security on Apple silicon; Apple no longer grants kext signing for device classes that have a user-space alternative. | Not viable. |
| **DriverKit system extension (dext)** | User space, IOKit-like C++ API | Supported. Needs an Apple Developer account and the `com.apple.developer.driverkit.transport.usb` entitlement, granted per vendor ID by Apple. | Unnecessary: no macOS kernel driver claims this device's interfaces, so nothing has to be displaced. |
| **Plain user-space USB** (IOUSBHost / IOUSBLib, or libusb on top of them) | The application's own process | Supported, no entitlement, no signing requirement. Works whenever no kernel driver has claimed the interface. | **Ideal.** All six interfaces are vendor-specific (class FF/FF/FF), so macOS leaves them unclaimed. Isochronous transfers are available from user space. |
| **CoreMediaIO Camera Extension** | A sandboxed system extension | The supported way to publish a system-wide camera (visible in QuickTime, FaceTime, browsers). Needs a paid Developer ID, a system-extension entitlement and notarisation. The older DAL plug-in mechanism is no longer loaded by current macOS. | The right *second* step, for use outside OBS. |

So a "macOS driver" for this device does not have to be a driver in the kernel sense at all. The device protocol and transport live in an ordinary library; the media glue is whichever host we target. For OBS specifically, the natural host is an **OBS source plugin**: OBS loads third-party plugins (its signature carries `disable-library-validation`), is not sandboxed, and takes uncompressed frames directly.

## 1.4 Why the Windows driver cannot be decompiled and recompiled for macOS

The original idea for this project was a builder that decompiles `elvidcap.sys` and recompiles it for macOS. It is worth being precise about why that cannot work, because the reasons shape the design.

1. **The binary is mostly Windows, not device.** Around two thirds of it is calls into `ks.sys`, `ntoskrnl.exe` and `usbd.sys`: IRPs, KS stream pointers, spin locks, registry keys. To recompile it you would have to supply a working reimplementation of those kernel interfaces on macOS, which is a far larger project than the driver itself (this is what NDISwrapper did for network drivers on Linux, for a much narrower API).
2. **Decompiler output is not source code.** Ghidra produces C-like pseudocode with invented variable names, no type information for most structures, and frequent artefacts (for example, in this binary a compile-time bit-field helper appears as dozens of meaningless shift loops). It is an excellent reading aid and a poor compiler input. Making it compile means understanding every function anyway.
3. **It would be the wrong architecture.** The code is x86-64 kernel code; the target is arm64 user space.
4. **It would redistribute Elgato's code.** A recompiled decompilation is a derivative of the vendor binary. A clean reimplementation of the *protocol* is not, and reverse engineering for interoperability with hardware you own is the well-established basis for exactly this kind of project.

What the decompilation is good for is the first row of the table in 1.1: recovering the device protocol. That is how it is used here.

## 1.5 What the builder is instead

The pipeline that replaces "decompile and recompile" keeps the same shape, with the recompile step replaced by a reimplementation that the decompilation verifies:

```
your installer.exe
   │  tools/unpack.sh            (7-Zip: NSIS installer -> driver files, split by architecture)
   ▼
analysis/unpacked/
   │  tools/decompile.sh         (Ghidra headless + tools/ghidra/ExportAll.java)
   ▼
analysis/ghidra/out/…           decomp.c, functions.tsv, strings.tsv, imports.tsv
   │  reading, cross-referencing  (docs/03, docs/appendix-*)
   ▼
docs/04-device-protocol.md      the hardware's behaviour, stated independently of any OS
   │  implementation
   ▼
src/libevc/                     user-space driver (C, libusb)
   │  make  /  tools/build-obs-plugin.sh
   ▼
out/evc-capture                 command-line capture tool
out/elgato-video-capture.plugin OBS source plugin
```

Everything derived from the vendor binary stays under `analysis/`, which is git-ignored.

## 1.6 An unusually lucky case: an open driver for the same chip

The decompilation identified the device as a **Conexant CX231xx ("Polaris")**, and the Linux kernel has carried an open-source driver for that chip family since 2009 (`drivers/media/usb/cx231xx`, with the decoder core in `drivers/media/i2c/cx25840`). Its board table even lists this product: `CX231XX_BOARD_ELGATO_VIDEO_CAPTURE_V2`, USB ID `0fd9:0037`.

Both the Linux driver and `elvidcap.sys` descend from Conexant's reference source, so function after function in the Ghidra output lines up with a named, commented Linux function. That changes the role of the reverse engineering:

- The **Linux source** supplies names, register definitions and intent.
- The **Windows decompilation** is the check on vendor-specific behaviour: what Elgato's build actually does on this board, where it differs from Linux, and what the INF configures.
- The **hardware** settles disagreements. Two things in this project were decided by measuring the device rather than by either driver (see 4.7 and 4.8 in the protocol chapter).

The consequence for licensing is that `src/libevc` is a port of GPL code and is itself **GPL-2.0-or-later**. OBS Studio is also GPL-2.0-or-later, so the plugin is compatible.

## 1.7 Reverse-engineering method used

1. **Unpack** the installer and identify each file's role from its type, version resource and the INF that references it.
2. **Read the INF first.** It gives hardware IDs, the service binary, and every registry value the driver will read. Those value names are then searchable strings in the binary.
3. **Read the device's descriptors from real hardware** (`out/lsdesc`). This fixes the interface and endpoint layout before any code is read.
4. **Auto-analyse in Ghidra headless** and export everything to text, so that the analysis is done with ordinary search tools and is reproducible.
5. **Anchor on constants.** Register addresses (`0x8d0`, `0x104`), USB request codes, IOCTL values (`0x220003`) and string references locate the important functions far faster than reading top-down. A function that writes `0x914 = 0xa0`, `0x918 = 0x1a0`, `0x808 = 0xf6` is identifiable at sight once the register map is known.
6. **Match against the open driver**, function by function, and record only the differences.
7. **Test every conclusion on the device**, with a tool that logs each register access (`evc-capture -vv`) and one that dumps the raw stream (`EVC_DUMP_STREAM`, `tools/usb/streamstat.py`).
