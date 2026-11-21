# 5. The macOS driver

## 5.1 Architecture

```
┌──────────────────────────── OBS Studio ────────────────────────────┐
│  elgato-video-capture.plugin   (src/obs-plugin/evc-obs.c)           │
│    source "Elgato Video Capture (analogue)"                         │
│    capture thread ──▶ obs_source_output_video (UYVY, BT.601)        │
│                  ──▶ obs_source_output_audio (S16LE stereo 48 kHz)  │
│  ┌───────────────────────────────────────────────────────────────┐ │
│  │ libevc  (src/libevc/evc.c)                                     │ │
│  │   bring-up, input/standard, picture controls                   │ │
│  │   isochronous streaming, BT.656 parser, field weave            │ │
│  └───────────────────────────────────────────────────────────────┘ │
│  libusb (compiled into the plugin)                                     │
└───────────────────────────────┬─────────────────────────────────────┘
                                │ IOUSBHost (user space, no kernel driver)
                         Elgato Video Capture 0fd9:0037
```

There is no kernel component, no system extension and nothing to approve in System Settings. The same library backs the command-line tool `out/evc-capture`.

| Path | What it is |
|---|---|
| `src/libevc/evc.h`, `evc.c` | The driver: about 1000 lines of C. Public API in `evc.h` |
| `src/tools/evc-capture.c` | Command-line capture and diagnostics |
| `src/obs-plugin/evc-obs.c` | OBS source plugin |
| `tools/build-obs-plugin.sh` | Builds the plugin against the OBS installed on the machine |
| `tools/usb/lsdesc.c`, `usbreset.c`, `streamstat.py` | Descriptor dump, port reset, raw-stream statistics |
| `Makefile`, `build.sh` | Build entry points |

## 5.2 Building

Requirements: Xcode command line tools and OBS Studio. Homebrew `libusb` is needed only for the command-line tools.

```sh
./build.sh                  # the plugin, plus the command-line tools if Homebrew libusb is installed
./build.sh install          # also copy the plugin to ~/Library/Application Support/obs-studio/plugins
./build.sh package 0.0.1-alpha    # build the installer: out/elgato-video-capture-0.0.1-alpha-macos-arm64.pkg
```

The plugin build (`tools/build-obs-plugin.sh`) fetches three things into `out/deps` the first time: the `libobs` headers for the exact OBS version installed (the plugin links against the `libobs.framework` inside `OBS.app`), SIMDe, which those headers need on Apple silicon, and the libusb source. libusb is built as a static library and linked in, so the plugin is one self-contained binary, built for macOS 13 and later (the oldest release OBS 33 supports) and ad-hoc signed. Each download is checked against a pinned SHA-256; for an OBS version the script does not know, it prints the checksum it got so that it can be added.

The installer (`tools/package.sh`) is a standard macOS package restricted to the current user's home directory, so it needs no administrator password. It is not signed with a Developer ID, which is why Gatekeeper asks for confirmation the first time; signing and notarising it would need a paid Apple Developer account.

To study the Windows driver as well (not needed to build or use the driver), put your own copy of the installer in the repository root and run `./build.sh analyse`, which additionally needs `brew install sevenzip ghidra openjdk@21`.

## 5.3 Using it in OBS

1. Quit OBS, run `./build.sh install`, start OBS.
2. Sources ▸ **+** ▸ **Elgato Video Capture (analogue)**.
3. In the source's properties choose the input (composite or S-Video). The standard is detected automatically when the device opens; it can be forced to NTSC or PAL. Brightness, contrast, saturation and hue adjust the decoder chip itself.
4. The picture is interlaced and woven, exactly as the Windows driver delivers it. Right-click the source ▸ **Deinterlacing** and pick a mode (Yadif 2x gives full motion at 59.94 or 50 fps). If motion looks jerky with deinterlacing on, switch the field order in the same menu.
5. Pixels are not square: 720×480 and 720×576 both represent a 4:3 picture. Use Transform ▸ Edit Transform and set the bounding box to a 4:3 size (for example 1440×1080) with "Stretch to bounds".

The source reopens the device automatically if it is unplugged and replugged, if USB transfers start failing, or if frames stop for three seconds. Frames with a missing or short field are not shown; OBS repeats the previous frame instead. Only one program can use the dongle at a time, so quit Elgato's own application first.

## 5.4 Command-line tool

```sh
out/evc-capture --info                          # initialise and report signal status
out/evc-capture --seconds 5 --ppm frame.ppm     # save the last frame as an image
out/evc-capture --frames 300 --raw clip.uyvy    # raw UYVY frames, back to back
out/evc-capture --seconds 10 --wav audio.wav    # audio
out/evc-capture --std pal --input svideo ...    # force a standard (default: auto), select input
out/evc-capture -vv --info                      # log every register access
EVC_DUMP_STREAM=dump.bin out/evc-capture ...    # raw bytes of the video endpoint
python3 tools/usb/streamstat.py dump.bin        # timing-code statistics for a dump
out/usbreset                                    # port-reset a device that stopped answering
```

A raw clip can be played or encoded with ffmpeg, for example:
`ffmpeg -f rawvideo -pix_fmt uyvy422 -s 720x480 -r 30000/1001 -i clip.uyvy -vf yadif out.mp4`.

## 5.5 What has been verified, and what has not

Tested on macOS 27.2 (Apple silicon) with one `0fd9:0037` unit, bcdDevice 40.02.

| Item | Result |
|---|---|
| Control path: every register read/write in the bring-up | Works; logged with `-vv` |
| **Composite, PAL, real source (a VHS tape)** | Correct picture: colour, geometry and burnt-in subtitles all as expected; 25.00 fps, 720×576, 0 isochronous errors over 16,000 packets |
| Audio from the same tape | 48 kHz stereo, both channels carrying programme audio (RMS about 2200 of 32768 on each) |
| Automatic standard detection | Picked PAL for the tape; with NTSC forced, no frames (as expected) |
| Composite, NTSC, no signal | 30 fps of the decoder's blue field, 0 errors |
| S-Video, no signal | 30 fps of the blue field, 0 errors |
| Pixel format | UYVY confirmed by the real picture |
| OBS plugin | Builds against OBS 33.0.2. With the tape playing, OBS's log reports "streaming PAL composite, 720x576" and the picture is visible in the preview with audio on the mixer meters |

Not yet verified, and the first things to check with a tape player or camera connected:

- **Field order and motion.** Only stills were inspected. Whether OBS's deinterlacer needs top or bottom field first is untested.
- **A/V sync, and left/right channel order.**
- **A real NTSC source.** NTSC uses the vendor's timing values, tested only without a signal. Whether the 240 lines kept per field are the right ones needs real content.
- **A real S-Video source.** The chroma input follows the vendor INF and differs from Linux (4.11).
- **Picture-control ranges** (brightness, contrast, saturation, hue).
- **Long runs**: hours of capture, hub and cable variations, sleep and wake.
- Other standards (PAL-M, PAL-N, PAL-60, NTSC-J, SECAM) are known from the reference drivers but not exposed.

## 5.6 Known limitations and next steps

1. **Timestamps** are the host clock at arrival, per transfer of 8 ms. That is what both reference drivers do. If lip-sync drifts or jitters in practice, derive video timestamps from a frame counter and audio timestamps from a sample counter, both anchored to the first arrival.
2. **Signal status** is not shown in OBS. `evc_get_signal()` exists and could drive a "no signal" indicator.
3. **Standard changes while running.** Detection happens when the device opens. If the source changes standard mid-session, frames stop and the plugin reopens the device after three seconds, which re-detects.
4. **System-wide camera.** To use the device in QuickTime, browsers or conferencing software, wrap `libevc` in a CoreMediaIO Camera Extension. That needs an Apple Developer ID, the system-extension entitlement and notarisation; the capture code would be reused unchanged.
5. **First-generation hardware** (`0fd9:0033`, eMPIA chip) is a different device and is not supported.
6. **Other colour systems.** Only NTSC-M and PAL-B/D/G/H/I are programmed. PAL-M, PAL-N, PAL-60, NTSC-4.43 and SECAM sources are captured with the right line count but the wrong colour subcarrier, so they will probably appear in black and white; the plugin logs a warning naming the detected format code. The per-format values are in Linux's `cx25840_std_setup`.
7. **Closed captions and VBI** are available from the device (4.8) and not captured.

## 5.7 Licence

The whole repository is **GPL-2.0-or-later** (`LICENSE`). For `src/libevc` that is a requirement, because it ports register sequences from the Linux `cx231xx` and `cx25840` drivers; the plugin links it, and the scripts and documentation are placed under the same licence for simplicity. OBS Studio is GPL-2.0-or-later. libusb (LGPL-2.1-or-later) is compiled from its unmodified release source and linked in. No Elgato code or firmware is included or required.
