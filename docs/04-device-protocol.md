# 4. Device protocol

This chapter states what the hardware expects and produces, independent of any operating system. It is the specification `src/libevc/evc.c` implements. Each section says where the information comes from: **L** = Linux `cx231xx` / `cx25840` source, **W** = the Windows driver decompilation (function addresses refer to the x64 build), **H** = measured on the device from macOS.

All multi-byte values are little-endian.

Where the two drivers disagree about this board, `libevc` uses the vendor's values, on the reasoning that Elgato configured its driver for this exact hardware while the Linux board entry is a community contribution. Section 4.11 lists every such place.

## 4.1 Bridge registers (L, W, H)

Vendor requests on the default control endpoint, recipient = device.

| Operation | bmRequestType | bRequest | wValue | wIndex | Data |
|---|---|---|---|---|---|
| Read register | 0xC0 | 0x0D | byte-enable mask | register | 1 to 4 bytes |
| Write register | 0x40 | 0x0C | byte-enable mask | register | 1 to 4 bytes |

The byte-enable mask is 0x1, 0x3, 0x7 or 0xF for 1 to 4 bytes. The driver always uses 4.

| Register | Name | Use |
|---|---|---|
| 0x00 | `BOARD_CFG_STAT` | Board strapping. Reads 0x000001E9 on the unit tested |
| 0x04 | `TS_MODE_REG` | Stream mode bits. OR in 0x100 for raw video |
| 0x18 | `EP_MODE_SET` | Endpoint enables: 0x04 = EP3 audio, 0x08 = EP4 video, 0x10 = EP5, 0x20 = EP6 |
| 0x74 | `PWR_CTL_EN` | Power sequencing. Bit 3 `PWR_AV_EN`, bit 6 `PWR_ISO_EN`, bits 5:4 mode (0x30 = external A/V), mode mask 0x17F |

## 4.2 I2C over USB (L, W, H)

The chip's internal blocks are reached through its I2C master 0.

| Operation | bmRequestType | bRequest | wValue | wIndex | Data |
|---|---|---|---|---|---|
| I2C write | 0x40 | 0x00 | see below | sub-address | 1 to 4 bytes |
| I2C read | 0xC0 | 0x04 | see below | sub-address | 1 to 4 bytes |

```
wValue = (7-bit slave address << 9) | (speed << 4) | (sub-address length << 2) | (no-stop << 1) | 1
speed: 0 = 1 MHz, 1 = 400 kHz, 2 = 100 kHz, 3 = 5 MHz        (100 kHz is used for the internal blocks)
bit 0 is "synchronous" and is always set
```

Requests 0x01/0x05 and 0x02/0x06 address masters 1 and 2 (external tuner and demodulator buses, unused here). Reads longer than 4 bytes must be split into 4-byte requests; nothing in this driver needs one.

Slaves: AFE 0x60 (2-byte sub-address), video decoder 0x88 (2-byte), audio ADC block 0x98 (1-byte). Addresses are given in 8-bit form; shift right by one for the 7-bit value in `wValue`.

## 4.3 Bring-up sequence (L, verified H)

This is the order `evc_init()` uses. Every step was observed to succeed on the device with `evc-capture -vv`.

1. **Power.** Read `PWR_CTL_EN`; clear the mode mask; write with `PWR_AV_EN`; wait 10 ms; add `PWR_ISO_EN`; wait 10 ms; add mode 0x30 (external A/V); wait 10 ms.
2. **AFE power.** Write AFE `0x08` (`SUP_BLK_PWRDN`) = 0x18 and read it back until it reads 0x18. Channel clamp/power registers `0x23`, `0x43`, `0x63` = 0x00. Audio ADC block `0x0F` = 0x00.
3. **AFE super block**, reference count 0x210: `0x01` = 0x10, `0x00` = 0x42, `0x05` = 0x0F, enable PLL (`0x08` = 0x18, confirm), start filter tuning (`0x02` = 0x40), wait 5 ms, `0x02` = 0x00. Then a revision-dependent tail (W, `FUN_00022688`): for bcdDevice below 0x4002 write `0x0A` = 0x72. For 0x4002 and above: `0x0A` = 0x52; move ADC 1's input select (`0x28` bits 5:4) to the next input; wait 10 ms; `0x21` = 1; wait 50 ms; `0x21` = 0; wait 50 ms; if bit 1 of `0x21` reads set, `0x0A` = 0x72; restore `0x28`; `0x27` = (value & 0xFD) | 1; `0x00` = 0xBC | (reference count bits 9:8).
4. **AFE channels 1 to 3** (register offsets +0x00, +0x20, +0x40): power up (`0x23` = 0x00), quantiser calibration (`0x0B` = 0x02), force modulator reset (`0x27` = 0x17), start calibration (`0x22` = 0x10), wait 5 ms, release reset (`0x27` = 0x07), pre-clamp for single-ended input (`0x29` = 0xF0), clear bits 7:3 of `0x2A`, dynamic element matching off (`0x26` = 0x03).
5. **Direct-IF block to baseband bypass** (it is only for tuners): decoder `0x340` = 0x0DF7DF83, set bit 31 of `0x33C`.
6. **Audio ADC block**: set bit 7 of `0x0E`, write `0x0F` = 0x00.
7. **Control pins**: decoder `0x120` (`PIN_CTRL`) |= ~0xFFFFFFC4, that is, set bits 0, 1, 3, 4, 5. On the unit tested this changed 0x80FFFF38 to 0x80FFFF3B.
8. **Decoder core reset and defaults**: pulse bit 0 of `0x102`; stop the audio microcontroller (clear bit 4 of `0x803`); `0x398` = 0; `0x002` = 0x76; `0x001` = 0x40; `0x33C` = 0x00000001; `0x340` = 0x0DF7DF83; luma `0x414` = 0x00107D12; chroma `0x420` = 0x3D008282; `0x102` = 0x10; `0x103` = 0x11; `0x400` = 0x00 (auto-detect); `0x401` = 0xE8. Then the standard timing table (4.5), start the microcontroller (set bit 4 of `0x803`), `0x404` = 0x0B, `0x42F` = 0x66, `0x474` = 0x1E1E601A.
9. **Input selection** (4.4), **line-in audio** (4.6), **standard** (4.5), **per-standard overrides** (4.5).

No firmware is uploaded at any point.

## 4.4 Video input selection (L, INF, H)

| Input | AFE mux | Decoder `0x103` | `MODE_CTRL` 0x400 bits 10:9 |
|---|---|---|---|
| Composite | Pin 2 to ADC channel 1: AFE `0x28` = 0x10 | 0xF1 | 0 (CVBS) |
| S-Video | Luma pin 1 to ADC 1 (`0x28` = 0x00), chroma pin 3 to ADC 2 (`0x48` = 0x20) | 0x01 | 1 (Y/C) |

The mux values are the INF's `XBarPin` `InputMux` entries (0x02 and 0x0301).

Common to both:

- Re-run the AFE super-block initialisation (4.3 step 3).
- `AFE_CTRL` (decoder 0x104): clear bits 24:15, set bit 24 (function mode = baseband), set bit 4. For composite also clear bit 5. For S-Video also set bit 12 (chroma from ADC 2) and clear bits 7 and 8 (channels 2 and 3 carry video, not audio).
- **`OUT_CTRL1` (0x404): set bit 7 and set bits 1:0 = 2 (VIP 1.1 output).** See 4.8.
- Direct-IF bypass again (4.3 step 5).
- `DFE_CTRL1` (0x488): set bit 27 (VBI gate) and bit 28 (automatic VGA).
- `MODE_CTRL` (0x400): set bit 5 (disable register auto-configuration), then the input mode bits above.
- `OUT_CTRL1`: set bit 3 (raw VBI lines in the active area); if bit 1 is set, also set bit 19.
- Decoder routing: `0x401` bits 2:1 = 0 for composite, 1 for S-Video; clear bits 2 and 1 of `0x102`.
- Audio path and input-change sequence from the decoder core: soft reset (set bit 0 of `0x810`), stop microcontroller, mute (`0x8D3` = 0x1F), `0x8D0` = 0x1F063870, sample-rate converters for 48 kHz (`0x900`, `0x904`, `0x90C` = 0x08014FAA), restart microcontroller, release reset; `0x402` = 0x04; `0x49F` = 0x14 (NTSC) or 0x11 (PAL); toggle bits 6:5 of `0x401` off then on; under soft reset write `0x808` = 0xF6 (525-line) or 0xFF (625-line) and `0x80B` = 0x00.
- `0x124` = 0x03, `0x914` = 0xA0, `0x918` = 0xA0, `0x919` = 0x01 (audio channel source and I2S framing). The Windows driver writes the same three values as one unit in `FUN_0001b000` (`0x914` = 0xA0, `0x918` = 0x1A0, `0x808` = 0xF6).

## 4.5 Video standard (L, H)

Format code in the low nibble of decoder `0x400`: 1 = NTSC-M, 4 = PAL-BDGHI. (Others exist: 2 NTSC-J, 3 NTSC-4.43, 5 PAL-M, 6 PAL-N, 7 PAL-Nc, 8 PAL-60, 0xC SECAM; `libevc` currently offers NTSC and PAL.) For PAL the decoder data sheet's procedure is to set NTSC-M first with the line comb off, then the PAL code, then toggle chroma AGC and chroma killer (bits 6 and 5 of `0x401`).

Timing table written to `0x470` to `0x47F`:

| Parameter | NTSC | PAL |
|---|---|---|
| Horizontal blanking | 122 | 132 |
| Horizontal active | 720 | 720 |
| Burst gate delay | 0x5B | 93 |
| Vertical blanking | 26 | 36 |
| Vertical active | 487 | 580 |
| Vertical blanking (656) | 26 | 40 |
| Source decimation | 0x21F | 0x21F |
| Luma / chroma low-pass | 1 / 1 | 2 / 1 |
| Comb filter | 0x66 | 0x20 |
| Subcarrier step | 556063 | 688739 |
| `0x47F` | 0x00 | 0x01 |

Because register auto-configuration is disabled, three registers are then overridden per standard:

| Register | Field | NTSC | PAL |
|---|---|---|---|
| `DFE_CTRL3` 0x490 | whole | 0xCD3F0280 | 0xCD3F0280 |
| `VERT_TIM_CTRL` 0x474 | bits 9:0 vertical blank | 0x18 | 0x24 |
| | bits 21:12 active lines | 0x1E6 (486) | not overridden |
| | bits 31:24 656 blank | 0x1E | not overridden |
| `HORIZ_TIM_CTRL` 0x470 | bits 9:0 horizontal blank | 0x79 | 0x85 |

The vendor driver waits 100 ms before these overrides (`FUN_0001d90c`).

Signal status is in `GEN_STAT` (decoder 0x40C): bit 21 video present, bit 16 horizontal lock, bits 11:8 detected format. The detected format is reported whatever standard is programmed (**H**: a PAL tape read as 4 while the decoder was set to NTSC), which is what `evc_autodetect_std()` uses. With the wrong standard programmed the stream contains field-1 codes only and no frame ever completes.

Picture controls (decoder, one byte each): brightness `0x414` (signed, 0 = neutral), contrast `0x415` (0x80 = neutral), saturation `0x420` and `0x421` (U and V, 0x80 = neutral), hue `0x422` (signed).

## 4.6 Line-in audio (L, H)

1. Audio ADC block: `0x0F` = 0x00, `0x0E` = 0x80.
2. Decoder, under soft reset (bit 0 of `0x810`): `0x124` (`AUD_IO_CTRL`) = 0; set bit 24 of `0x91C` (bypass the 2× upsampler); `0x910` = 0; `0x800` = 0x03000001; `0x8D0` (`PATH1_CTL1`) = 0x00063073 (unmute); OR 0x7FFF0000 into `0x8D4` and `0x8DC`.

## 4.7 Starting and stopping streams (L, W, H)

Video:

1. `SET_INTERFACE`: interface 3, alternate setting 3.
2. Queue isochronous IN transfers on endpoint 0x84, 2892 bytes per microframe. (The vendor driver keeps 8 transfers of 80 microframes queued; `libevc` uses 8 × 64.)
3. Bridge `TS_MODE_REG` |= 0x100, then `EP_MODE_SET` |= 0x08.

Audio:

1. `SET_INTERFACE`: interface 2, alternate setting 1.
2. Queue isochronous IN transfers on endpoint 0x83, 28 bytes per microframe.
3. `EP_MODE_SET` |= 0x04.

Linux additionally ORs 0x9300 into `TS_MODE_REG` when starting audio. The Windows driver does that only for audio embedded in the ancillary stream, which this board does not use (`FUN_00026714`). **H:** audio streams correctly without it, so `libevc` follows the vendor driver.

To stop: clear the bits in `EP_MODE_SET`, cancel the transfers, select alternate setting 0 on both interfaces.

Choosing the wrong packet size is not harmless. Queueing isochronous transfers sized from alternate setting 0 (a 512-byte bulk placeholder) left the device unable to answer control requests until it was port-reset (`out/usbreset`). **H.**

## 4.8 Video stream format (L, W, H)

There is **no per-packet header**. Endpoint 0x84 carries one continuous byte stream, split arbitrarily across microframes:

```
FF 00 00 XY  <1440 bytes: one line, 720 pixels, U Y V Y ...>  FF 00 00 XY  <1440 bytes> ...
```

| XY & 0xF0 | Meaning |
|---|---|
| 0x80 | Start of an active line, field 1 |
| 0xC0 | Start of an active line, field 2 |
| 0x20 / 0x60 | Raw VBI line, field 1 / 2 (only when enabled) |
| 0x90, 0xD0, 0xB0, 0xF0 | End-of-line codes; an ancillary packet (`00 FF FF …`) may follow |

Measured on the device with composite input, NTSC, no ancillary data enabled: the stream consists only of 0x80 and 0xC0 codes, each exactly 1444 bytes after the previous one, about 243 lines per field and 30 field pairs per second. In PAL mode the frame rate is 25.

Rules for a parser, all of which both drivers share and `parse_video()` implements:

- A timing code can straddle two packets; carry state across packet boundaries.
- Once a start code is seen, the next 1440 bytes are line data and are not searched for codes.
- If the 4 bytes after a start code are themselves an end-of-line code, the line is empty and must be skipped.
- Only the first `height / 2` lines of each field are kept (240 for NTSC, 288 for PAL); the decoder sends a few more.
- Field 1 goes to even rows of the frame, field 2 to odd rows: the result is a **woven interlaced frame**.
- A frame is complete when field 2 has all its lines and field 1 had all of its. Otherwise it is dropped.

**Byte order is UYVY** (Cb, Y, Cr, Y). The Windows driver copies the bytes unchanged into a buffer it advertises as FOURCC `UYVY` (copy routine at `0x27920`); Linux swaps each pair and advertises `YUYV`. On macOS this is `kCVPixelFormatType_422YpCbCr8` (`'2vuy'`) and OBS's `VIDEO_FORMAT_UYVY`, with no conversion. With no signal connected the decoder outputs a constant `D4 23 72 23` (U = 212, Y = 35, V = 114), which decodes to the familiar "no signal" blue, confirming the order.

Colour is BT.601, limited range (16 to 235).

**A hardware finding.** Bit 7 of the timing codes depends on `OUT_CTRL1` bit 7. Linux sets that bit only in its composite path and relies on composite having been selected at least once since power-up; the vendor driver sets it in its common input-select routine (`FUN_0001cd00`). When S-Video was selected directly after power-up the device sent lines tagged 0x00 and 0x40 instead of 0x80 and 0xC0, and a parser following either driver's rules produced no frames at all. `libevc` therefore sets `OUT_CTRL1` bit 7 and the VIP 1.1 output mode for both inputs.

## 4.9 Audio stream format (L, W, H)

Endpoint 0x83 carries raw PCM with no header: signed 16-bit little-endian, 2 channels interleaved, 48,000 frames per second. Packets hold a whole number of 4-byte frames, about 24 bytes per microframe. Measured: 143,993 frames in 3.00 s.

The device supplies no timestamps for either stream and the drivers do no A/V synchronisation; both stamp samples with the host clock on arrival.

## 4.11 Where the vendor driver and Linux disagree (W)

Details and function addresses are in appendix A, section 3. "Used" is what `libevc` does.

| Item | Vendor (Windows) | Linux | Used |
|---|---|---|---|
| AFE reference count | 0x210 | 0x23C | Vendor |
| AFE super-block tail | Revision-dependent calibration (4.3 step 3) | None | Vendor |
| ADC channel 3 power in external-A/V mode | On (0x00) | Off (0x40) | Vendor |
| AFE input-mux write | Whole register | Bits 5:4 only | Vendor |
| S-Video chroma input | Pin 3 (mux 0x0301) | Pin 1 (mux 0x0101) | Vendor. **Untested: no S-Video source was available** |
| NTSC vertical timing | 486 active lines, 656 blank 0x1E | 487, 0x1C | Vendor |
| PAL vertical timing | Blank 0x24 only | Blank 0x20, active 0x244, 656 blank 0x24 | Vendor. Verified with a PAL tape |
| `TS_MODE_REG` for audio | Not written | \|= 0x9300 | Vendor. Verified |
| `DL_CTL` 0x800 in the line-in branch | Not written there (0x03000000 once at start) | 0x03000001 | Linux |
| I2C clock for internal blocks | 400 kHz | 100 kHz | Linux |
| First power mode | 0x20, then 0x30 on input select; 5 ms steps | 0x30 directly; 10 ms steps | Linux |
| Tuner AGC GPIO 28 | Driven low at start | Untouched | Linux (no tuner fitted) |
| Decoder initialisation and standard select | Lets the chip auto-configure for the format, then overrides | Writes the timing table explicitly (4.5) | Linux |
| Output framing bit (4.8) | Set on every input select | Composite path only | Vendor |
| Vendor requests | Up to 4 attempts, 5 s timeout | One attempt, 1 s | Linux |

The rows marked Linux are places where the port was already working on the hardware and the vendor's variant was not needed; they are candidates to revisit if a problem appears.

## 4.10 Scaling (W, not implemented)

The decoder has a hardware scaler that the vendor driver uses for output sizes below full resolution (`FUN_0001d4f0`): `0x418` = ((720 − w) << 20) / w and `0x41C` = −(((H − h) << 9) / h) & 0x1FFF, with a narrower alternate setting for narrower pictures. `libevc` always captures the full 720×480 or 720×576 and leaves scaling to the host application.
