# 2. The installer and the hardware

## 2.1 The installer

`Video_Capture_Driver_Installer.exe` (3,220,128 bytes, SHA-256 `3ba345bb…b463f7`) is a Nullsoft (NSIS 2.45.1 Unicode) self-extractor. 7-Zip unpacks it into 34 files: a generic `setup.exe` driven by `setup.ini`, and two parallel sets of driver files, one for x64 and one for x86, with identical names. `tools/unpack.sh` separates them into `analysis/unpacked/x64` and `analysis/unpacked/x86`.

`setup.ini` reports product version **7.07.0.127** and installs two INF files. That is the key finding of this stage: **the installer carries two unrelated drivers for two hardware generations sold under the same name.**

| | First generation | Second generation |
|---|---|---|
| USB ID | `0fd9:0033` | **`0fd9:0037`** |
| INF | `Elgato_Video_Capture.INF` (2010-01-27, v5.9.1202.0) | `Elgato_Video_Capture_2010.inf` (2014-09-22, v7.07.0.127) |
| Chip vendor | eMPIA (EM28xx family) | **Conexant (CX231xx "Polaris")** |
| Kernel driver | `emBDA.sys` / `emBDA64.sys`, `emOEM.sys` / `emOEM64.sys` | **`elvidcap.sys`** (same name for both architectures) |
| Service name | `USB28xxBGA`, `USB28xxOEM` | `CXPLRCAP` (display name "EVC2010") |
| User-mode parts | `emPRP.ax`, `emmon.exe` | `CxPolaris.ax`, `cpnotify.ax`, `y8cnvt.ax`, `cxtvrate.dll` |
| Firmware file | `merlinFW.rom` (16,382 bytes, dated 2006) | none shipped (see 2.4) |
| Linux driver | `em28xx` | `cx231xx` |

The device this project was developed against is the second generation, so everything from here on concerns `elvidcap.sys`. A first-generation dongle would need a separate driver (the Linux `em28xx` driver would be the starting point); `libevc` refuses nothing explicitly but simply will not find a `0fd9:0037` device.

`elvidcap.sys` details:

| | x64 | x86 |
|---|---|---|
| Size | 153,448 bytes | 122,984 bytes |
| SHA-256 | `494fda99…653e9956` | `dc3e58e0…8d9ce3a1` |
| Version resource | 7.07.00.127, "Videocapture driver", © 2011 Elgato Systems GmbH | same |
| Debug path | `d:\svn\elgatowin\trunk\kernel\polaris\avs_capture\objfre_win7_amd64\amd64\elvidcap.pdb` | |
| Functions found by Ghidra | 401 | 643 |

The debug path confirms the lineage: `polaris` is Conexant's code name for the CX231xx, `avs_capture` is the AVStream capture driver from Conexant's reference kit, built with the Windows 7 DDK. No symbols are included.

## 2.2 What the INF configures

The INF binds the driver to interface 1 of the composite device (`USB\VID_0FD9&PID_0037&MI_01`) and installs a null service on interface 0 (`MI_00`, the infrared/control interface, described as "Elgato Helper Device"). A commented-out pair of lines shows the prototype hardware ID, `VID_1F4D&PID_0102`.

Registry values written under the device's `DriverData` key, with their meaning where known:

| Value | Data | Meaning |
|---|---|---|
| `BoardType` | 0x0A | Conexant reference board type; the INF comment names both 9 and 10 "RDU253S" |
| `TunerType` | 0x28 | NXP 18271 in the reference design; no tuner is fitted |
| `NoOfTuners` | 0 | No tuner |
| `Enable_BDA` | 0 | Digital TV support off |
| `VideoStandard` | x64 INF: 0x01, x86 INF: 0x20 | Default analogue standard. The two INFs differ; the change log says "Set Default Video Standard to NTSC" (2013) |
| `EnableAutoFormatDetection` | 1 | Let the decoder detect the standard |
| `VideoInputPin` | 0 | Default video input: crossbar pin 0 |
| `AudioInputPin` | 2 | Default audio input: crossbar pin 2 (line in) |
| `ForceAudioWithVideoPin` | 1 | Audio follows the video input selection |
| `ctrl_pin_status_mask` | 0xFFFFFFC4 | Which decoder control pins are driven. Identical to the Linux board entry |
| `gpio_pin_status_mask` | 0x0400000A | GPIO pins in use. Linux has 0x04001000 for this board |
| `AGC_Analog_Digitial_Select_Gpio_Bit` | 0x1C | Tuner AGC select GPIO (unused without a tuner). Linux has 0x0C |
| `EnableHANCAudioOut` | 0 | Audio goes out on its own endpoint, not embedded in the video ancillary stream |
| `CpMethod` | 0xFE | Copy-protection method. 0xFE prevents the driver's monitor thread from being created |
| `I2CSpeed` | 1 | 400 kHz for the external (tuner) I2C bus |
| `Enable_SW_Encoder` | 1 | Expose the uncompressed path (no hardware MPEG encoder is fitted) |

Crossbar (input routing) pins:

| Pin | `PinType` | `InputMux` | Meaning |
|---|---|---|---|
| `XBarPin0` | 2 (composite) | 0x02 | Composite video on analogue front-end input **VIN2_1** |
| `XBarPin1` | 0 (S-Video) | 0x01, 0x03 | Luma on **VIN1_1**, chroma on **VIN1_2** (byte 1 = 0x03 selects the Y/C pairing) |
| `XBarPin2` | 4 (audio line in) | 0x00 | Line-level audio, `GpioMask` 1, `GpioSettings` 0 |

This matches the Linux board entry exactly (`CX231XX_VIN_2_1` for composite, `CX231XX_VIN_1_1 | CX231XX_VIN_1_2 << 8` for S-Video), which is good independent confirmation of the input wiring.

## 2.3 USB descriptors, read from the device

Read with `out/lsdesc` on macOS. High-speed device, one configuration, 500 mA, six vendor-specific interfaces.

```
device 0fd9:0037  bcdDevice 40.02  bcdUSB 2.00  class EF/02/01 (interface association)  EP0 64 bytes
strings: "Elgato" / "Video Capture" / serial

if 0  alt 0   EP 8E IN  interrupt  32 bytes     infrared / control
              EP 0E OUT interrupt  32 bytes
if 1  alt 0   EP 8F IN  interrupt   8 bytes     status / GPIO events
if 2  alt 0   EP 83 IN  bulk      512 bytes     AUDIO
      alt 1   EP 83 IN  isoc       28 bytes       <- used: 48 kHz stereo PCM
      alt 2   EP 83 IN  isoc       52 bytes
if 3  alt 0   EP 84 IN  bulk      512 bytes     VIDEO
      alt 1   EP 84 IN  isoc      184 bytes       width <= 180
      alt 2   EP 84 IN  isoc      728 bytes       width <= 360
      alt 3   EP 84 IN  isoc  3 x 964 = 2892      <- used: full width
      alt 4   EP 84 IN  isoc  2 x 900 = 1800      never selected by the vendor driver
if 4  alt 0   EP 85 IN  bulk      512 bytes     transport stream (digital TV, unused)
      alt 1   EP 05 OUT bulk      512 bytes
if 5  alt 0   EP 86 IN  bulk      512 bytes     ancillary: sliced closed captions / HANC
      alt 1   EP 86 IN  isoc      576 bytes
```

Every interface is class FF/FF/FF, so macOS attaches no driver of its own and a user-space program may claim them.

Two numbers explain the quality difference this project is after:

- Video alternate setting 3 is a high-bandwidth isochronous endpoint: 3 × 964 bytes in each of 8000 microframes per second, **23.1 MB/s** of capacity.
- Uncompressed 720×480 4:2:2 at 29.97 fps needs 720 × 480 × 2 × 29.97 = **20.7 MB/s**; 720×576 at 25 fps needs the same.

The device therefore delivers every pixel uncompressed at 16 bits per pixel. Nothing in the hardware limits bit rate; any limit seen in capture software is applied by that software's encoder.

## 2.4 Firmware

The driver contains two firmware file names, and neither file is in the installer:

| Path in the binary | Purpose | Shipped? |
|---|---|---|
| `\systemroot\system32\drivers\merlinD.rom` | Code for the decoder's audio microcontroller (broadcast audio standard detection, needed for TV tuner sound) | No. The INF's `[SourceDisksFiles]` lists `merlind.bin`, but no `CopyFiles` section references it and it is not in the archive |
| `\systemroot\system32\drivers\cx416enc.rom` | Firmware for an optional CX23416 hardware MPEG encoder | No. The board has no such chip |

The driver opens `merlinD.rom` with `ZwCreateFile` and only attempts the upload if the open succeeds, then continues with initialisation either way. Linux behaves the same with its equivalent file (`v4l-cx231xx-avcore-01.fw`). The measurements in chapter 5 were made with no firmware loaded, which confirms that **this board needs no firmware upload** for composite, S-Video or line-in audio.

(`merlinFW.rom`, which *is* in the installer, belongs to the first-generation eMPIA driver.)

## 2.5 Inside the chip

The CX231xx is a single chip that behaves, from the host's point of view, like a USB bridge with several I2C devices behind it:

```
                      ┌──────────────────────────── CX231xx ────────────────────────────┐
 composite ──VIN2_1──▶│ AFE "Colibri"        Video decoder "Merlin"      USB bridge      │
 S-Video Y ──VIN1_1──▶│ I2C 0x60             I2C 0x88 (cx25840-like)     EP0 vendor reqs │── USB 2.0 HS
 S-Video C ──VIN1_2──▶│ 3 ADC channels  ───▶ comb filter, scaler,   ───▶ bridge registers│
                      │ input mux, PLL       BT.656/VIP output           EP 0x84 video   │
 line L/R ───────────▶│ Audio ADC "Flatiron" ─▶ audio path, SRC     ───▶ EP 0x83 audio   │
                      │ I2C 0x98                                                          │
                      └──────────────────────────────────────────────────────────────────┘
```

| Block | I2C address (8-bit) | Sub-address width | Role |
|---|---|---|---|
| Bridge | (not I2C; vendor requests 0x0C / 0x0D) | 16-bit register number | Power sequencing, endpoint enables, stream mode, GPIO |
| AFE | 0x60 | 2 bytes | Analogue front end: reference PLL, three ADC channels, input multiplexers |
| Video decoder | 0x88 | 2 bytes | Standard decoding, picture controls, timing, output format, audio routing, direct-IF block (tuner only) |
| I2S / audio ADC | 0x98 | 1 byte | Line-in ADC power and clocking |

The bridge register `BOARD_CFG_STAT` (0x00) reads **0x000001E9** on this unit.
