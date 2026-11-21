> **Appendix: working notes on the streaming path of `elvidcap.sys`.** Written while reading the Ghidra output, kept as
> evidence for chapters 3 and 4. Paths under `analysis/` exist only after running `./build.sh analyse` with your own
> copy of the installer. Facts settled later on the hardware: the test unit's bcdDevice is 0x4002; the Linux `cx25840`
> decoder driver was compared separately (chapter 4); composite PAL capture works with the vendor values listed here.

# elvidcap.sys (x64, v7.07.00.127) — streaming data path

Scope: USB streaming only (alt settings, URBs, packet parsing, KS pins, timestamps).
Register init is out of scope. Source: Ghidra export `analysis/ghidra/out/x64_elvidcap_sys/`
plus raw bytes of `analysis/unpacked/x64/elvidcap.sys` (image base 0x10000, file offset =
address - 0x10000 for all sections). Small functions Ghidra did not export were read with
`objdump -d` and are cited by raw address (e.g. `0x279c0`).

Conventions: every claim cites a function/address. "inferred" = not read directly from code.
Linux names refer to `analysis/reference/linux-cx231xx/`.

---

## 0. Object model (needed to read the rest)

| Object | Created in | Key fields |
|---|---|---|
| Device context | FUN_00014048 | +0x50 video decoder obj (vtable 0x2bdb0), +0x58 audio obj, +0x60 copy-protection monitor, +0xb0 video stream, +0xb8 audio stream, +0xc0 VANC stream, +0xc8 HANC stream, +0xd0 TS1, +0xd8 TS2, +0x158+8*type = pin currently owning each stream type, +0x1a4 current pcb config index, +0x2b8/+0x2b9 current video/audio alt |
| USB object (dev+0x28) | FUN_00018934 / FUN_000192bc | +0x10 device descriptor (18 bytes), +0x28 config descriptors, +0x38 per-interface USBD_INTERFACE_INFORMATION copies, +0x48 "needs pipe reset" flag, +0xb0 "device gone" flag |
| Stream object, 0x3f8 bytes, vtable 0x2c300 | FUN_00025778 -> FUN_00025858 | +0x20 pipe index, +0x24 interface index, +0x28 URB size, +0x2c transfer buffer size, +0x30 stop flag, +0x38 thread, +0x48 video line assembler, +0x110 raw audio sink, +0x178 VBI line assembler, +0x280 URB count, +0x288 completed-URB queue, +0x2c0 TS sink, +0x3e0 number of URBs, +0x3e5 stream type, +0x3e8 EP_MODE_SET mask, +0x3ec bulk flag |
| Pin object (video 0xb0 bytes vtable 0x2bb10; audio 0x40 bytes vtable 0x2bab0; VBI 0x88 bytes vtable 0x2bb70) | FUN_00016848 / FUN_000164a0 / FUN_00016f60 | +0x08 picture number, +0x10 drop count, +0x18 device ctx, +0x20 PKSPIN, +0x28 IKsReferenceClock, +0x30 "first sample" flag, +0x38 sample duration |

Stream vtable 0x2c300 (read from .rdata): +0x00 dtor FUN_00025bac, +0x08 attach pin FUN_000269fc,
+0x10 detach pin FUN_00026abc, +0x18 start FUN_00025c3c, +0x20 stop FUN_00026230,
+0x28 format-changed FUN_00025820, +0x38 set interface index `0x26cdc`, +0x40 FUN_000260b0 (bulk
firmware push for the cx416 encoder, not used for raw capture).

Stream type byte (+0x3e5), from FUN_00025858: 0 = raw video, 1 = audio, 2 = VANC/VBI,
3 = HANC/sliced CC, 4 and 6 = TS1, 5 = TS2, 7 = second VANC-like stream, 0x81 = audio carried
in HANC.

Pin type (pin vtable +0x18): 0 video (`0x256d4` = `xor eax,eax; ret`), 1 audio (`0x167b0` returns 1), 2 VBI (`0x172e4` returns 2), 4 TS1, 5 sliced CC,
6 TS2 (FUN_00014e34 / FUN_0001515c switch).

---

## 1. USB alternate settings and URB geometry

### 1.1 Interface table

- FUN_00019898 builds the SELECT_CONFIGURATION request: it calls
  `USBD_ParseConfigurationDescriptorEx(cfg, cfg, i+1, 0, -1,-1,-1)` for i = 0..bNumInterfaces-1
  when `bcdDevice != 0` (the word at USB object +0x1c = device descriptor offset 12), or with
  all wildcards when `bcdDevice == 0`; then `USBD_CreateConfigurationRequestEx`, submits it
  (FUN_000189d0) and stores one USBD_INTERFACE_INFORMATION copy per interface (FUN_00019814).
- Consequence, read in FUN_00019b30: **driver interface index N maps to bInterfaceNumber N+1**
  whenever bcdDevice != 0 (`bVar10 = param_2 + 1`). The INF binds the driver to `MI_01`
  (Elgato_Video_Capture_2010.inf line 47), which is consistent.
- Which index is "video"/"audio" comes from the pcb-config table at (dev+8)+0x50. Its layout is
  byte-for-byte the Linux `struct pcb_config` (cx231xx-pcb-cfg.h): +1 power type, +2 speed
  (1 = high speed), +0x14 config count, 9-byte `hs_config_info[]` records at +0x15
  (+0x16 interrupt, +0x17 ts1, +0x18 ts2, +0x19 audio, +0x1a video, +0x1b vanc, +0x1c hanc),
  `fs_config_info[]` at +0x30. Offsets read in FUN_00015e34 (0x1a/0x35), FUN_00015f48
  (0x19/0x1c/0x34), FUN_00016030 (0x17/0x32), FUN_00016100 (0x18/0x33), FUN_000161e0 (0x1b),
  FUN_0001626c (0x1c).
- Not resolved here: the actual index values for this board (the table is selected at init,
  out of scope). With the hardware descriptors (interface 2 audio, 3 video) the indices must be
  audio = 1, video = 2 — inferred, not read.

### 1.2 SELECT_INTERFACE

FUN_00019b30(usb, ifIndex, alt), reached only through the locking wrapper FUN_0001e5e0:
1. Parses `(interface = ifIndex+1, alt = 1)`; if that alternate does not exist it returns
   success for alt 0 and failure otherwise.
2. Parses `(interface, alt)`, allocates a URB of `0x50 + (numEndpoints-1)*0x18` bytes, function
   code 1 (URB_FUNCTION_SELECT_INTERFACE), copies the config handle from usb+0x40, submits via
   FUN_000189d0 and replaces the cached interface info (FUN_00019814).

### 1.3 Which alternate is chosen

| Stream | Function | Rule |
|---|---|---|
| Video | FUN_00015e34 | Start (arg != 0): asks the decoder for the current output width (decoder vtable +0x20 = `0x1bf1c`). High speed: width > 360 -> **alt 3**; 181..360 -> alt 2; 1..180 -> alt 1; 0 -> alt 0. Full speed: alt 1. Stop (arg == 0 and current alt != 0): alt 0. Result cached at dev+0x2b8. |
| Audio | FUN_00015f48 | Start: **alt 1**; stop: alt 0. Interface index = audio_index, or hanc_index when registry `EnableHANCAudioOut` != 0 (dev+0x44). Skipped if already at that alt (dev+0x2b9). |
| VANC / VBI | FUN_000161e0 | Always alt 0, high speed only. |
| HANC / sliced CC | FUN_0001626c | Always alt 0, high speed only. |
| TS1 / TS2 | FUN_00016030 / FUN_00016100 | High speed: alt 4 to start, alt 0 to stop; full speed: alt 1. |

For a full-width capture this means video interface alt 3 (isoc 964 x 3) and audio interface
alt 1 (isoc 28). Alt 4 of the video interface (900 x 2) is never selected by this driver.
Identical thresholds to Linux `cx231xx_set_video_alternate` (cx231xx-core.c:456-482); audio
alt 1 matches cx231xx-audio.c:414.

Call order on the video pin going to RUN (FUN_0001e4b4, state 3): pin vtable +0x20
(FUN_000169c8: programs the scaler with the negotiated width/height via decoder vtable +0x10 =
FUN_0001d4f0) -> FUN_00014e34 -> FUN_00015e34 (select alt) -> stream attach (+0x08) -> stream
start (+0x18). On failure the pin is detached again.

### 1.4 Endpoint enable / mode registers written at stream start

FUN_00026714 (called first thing by FUN_00025c3c), high speed:
- video (type 0): vendor register 0x04 (Linux `TS_MODE_REG`) |= 0x100
- VANC (2): |= 0x300; HANC (3): |= 0x1300; HANC audio (0x81): |= 0x9300
- **audio (type 1): no TS_MODE_REG write at all**
- full speed, any type: |= 0x101

then FUN_00026b6c ORs the stream mask (+0x3e8) into vendor register 0x18 (Linux `EP_MODE_SET`):
video 0x08, audio 0x04, VANC 0x10, HANC and HANC-audio 0x20, TS1 0x01, TS2 0x02 (masks assigned
in FUN_00025858). FUN_00026be0 clears the same bit on stop. Read-modify-write helper is
FUN_00026c58; register read/write are FUN_0001e838 / FUN_0001e920.

### 1.5 Isochronous URB geometry

- Packets per URB: FUN_00026714 sets `0x50` (**80**) at high speed, 2 at full speed.
- Transfer buffer size (+0x2c) = 80 * `USBD_PIPE_INFORMATION.MaximumPacketSize`;
  URB size (+0x28) = 80*12 + 0x98 = 0x458 bytes.
- FUN_00019dc0 builds the URB: function 0x0A (URB_FUNCTION_ISOCH_TRANSFER),
  `NumberOfPackets = bufferSize / MaximumPacketSize`, TransferFlags 7 for an IN pipe
  (IN | SHORT_OK | START_ISO_TRANSFER_ASAP), `IsoPacket[i].Offset = i * MaximumPacketSize`.
- URBs in flight (+0x3e0, FUN_00025858): **video 8, audio 4**, VANC 2, HANC 2, TS 8.
  FUN_00025c3c allocates that many {0x88-byte context, transfer buffer, IRP, URB} sets and
  submits each immediately through FUN_00026350 -> FUN_00018d68 -> FUN_00019dc0.
- 80 microframes = 10 ms per URB; video has 80 ms of URBs queued, audio 40 ms.
- Inferred (Windows USB stack behaviour, not in this binary): for the high-bandwidth alt 3 the
  stack reports MaximumPacketSize = 964*3 = 2892, giving a 231,360-byte video buffer per URB;
  audio = 80*28 = 2,240 bytes.
- Bulk pipes (PipeType 2, used by VANC/HANC alt 0): one buffer of 0x200*0x140 = 163,840 bytes
  (0x200*2 for type 1, 0x200*8 for type 7), URB function 9, built in FUN_00019fdc; flag +0x3ec
  makes the worker treat the buffer as one packet.

Linux differs: 40 packets per URB and 8 URBs for video (`CX231XX_NUM_PACKETS`,
`CX231XX_NUM_BUFS`), 64 packets and 5 URBs for audio.

### 1.6 Completion and resubmission

- Isoc completion routine FUN_0001a190: walks `IsoPacket[]`, summing `Length`, and stops at the
  first packet whose Status is negative and is not 0xC0000011 or 0xC0020000
  (USBD_STATUS_ISO_NOT_ACCESSED_BY_HW); calls the context callback with (that status, byte sum).
- Callback FUN_000263e0: if status == 0 and sum != 0 the context's "valid length" (+0x28) is set
  to the sum, otherwise to 0; the context is appended to the stream's completed queue
  (FUN_00026d58, spin-locked list + event).
- No parsing happens at DISPATCH level. All parsing is in the stream thread (section 5).

---

## 2. Video packet parsing

Call chain: FUN_0002644c (thread) -> FUN_000277bc (per packet) -> FUN_00027430 (per SAV/EAV
code) -> FUN_00026f2c (line accounting) -> `0x27920` (copy) / `0x279c0` (frame done) ->
FUN_0001e37c (complete KS sample).

### 2.1 Per-packet framing

- For every `IsoPacket[i]` with `Length != 0`, the thread passes
  `buffer + IsoPacket[i].Offset, Length` to FUN_000277bc (FUN_0002644c). **Per-packet Status is
  not checked here**; the only filter is Length != 0.
- **There is no per-packet header.** Byte 0 of a packet is stream payload; nothing is skipped
  (FUN_000277bc starts scanning/copying at offset 0). Same as Linux.
- The stream is ITU-R BT.656-style: 4-byte timing codes `FF 00 00 XY` followed by payload.

### 2.2 FUN_000277bc — packet walker (Linux: `cx231xx_isoc_copy` body)

State in the line assembler (stream+0x48): +0x22a "partial" flag, +0x228 last code,
+0x22c last 4 bytes of the previous packet.

1. If partial flag set: resume with the saved code (+0x228).
2. Else, if the packet starts with `00 FF FF`: treat as code 0xB0, i.e. go straight to the
   ancillary scanner (Windows only).
3. Else build `[last 4 bytes of previous packet | first 4 bytes of this one]` and look for
   `FF 00 00 XY` with XY != 0xA5; if found, consumed = position - 4. This is
   `cx231xx_find_boundary_SAV_EAV`.
4. Only the high nibble of XY is used (`& 0xF0`), as in Linux.
5. Loop: call FUN_00027430 for the current code, then scan forward for the next
   `FF 00 00 XY` (inline equivalent of `cx231xx_find_next_SAV_EAV`), until the packet is
   exhausted.
6. Save the packet's last 4 bytes to +0x22c.

The inline scan stops 4 bytes before the end (`i < remaining - 4`), so a code occupying exactly
the last 4 bytes is found by step 3 of the next packet instead. Net effect is the same as Linux.

### 2.3 FUN_00027430 — code dispatch (Linux: `cx231xx_get_video_line`)

| Code (& 0xF0) | Linux name | Windows action |
|---|---|---|
| 0x80 | SAV_ACTIVE_VIDEO_FIELD1 | video line, field index 0 |
| 0xC0 | SAV_ACTIVE_VIDEO_FIELD2 | video line, field index 1 |
| 0x20 | SAV_VBI_FIELD1 | VBI line, field 0 (assembler at +0x130) |
| 0x60 | SAV_VBI_FIELD2 | VBI line, field 1 |
| 0x90, 0xD0, 0xB0, 0xF0, 0x30, 0x70 | EAV_* | ancillary-packet scanner (section 6) |
| 0xA5 | — | internal marker: ancillary packet continues from previous USB packet |
| 0xA0, 0xE0, anything else | SAV_VBLANK_* | ignored, 0 bytes consumed |

For 0x80/0xC0, if the payload immediately begins with `FF 00 00` + {0x90, 0xD0, 0xB0, 0xF0} the
line is treated as empty and skipped — the same "skipped line" test as Linux.

### 2.4 FUN_00026f2c — line accounting (Linux: `cx231xx_copy_video_line`)

Line assembler fields: +0x08 current KS stream-pointer clone, +0x10 lines completed,
+0x14 bytes left in line, +0x18 line length, +0x1c destination pitch, +0x20 lines per field,
+0x24 current field (0/1, initial 3), +0x28 pin, +0x70 field-1-done.

- Field change (`current != requested`): vtable[0] = FUN_00027a68 (Linux
  `cx231xx_reset_video_buffer`): when leaving field index 0, `field1_done = lines_completed >=
  lines_per_field`; if there is no buffer, take one (FUN_0001e300) and clear field1_done;
  `lines_completed = 0`, `bytes_left = line_length`.
- `n = min(bytes available, bytes_left)`.
- If `lines_completed >= lines_per_field`: consume nothing, just decrement bytes_left (extra
  lines are discarded).
- If no buffer: report n consumed, decrement bytes_left, copy nothing.
- Otherwise copy n bytes (vtable[1] = `0x27920`), decrement bytes_left. When it hits 0:
  `lines_completed++`, `bytes_left = line_length`, partial = 0, and if vtable[2] (`0x279c0`)
  says the frame is done: `DataUsed = FrameExtent`, FUN_0001e37c completes the sample, buffer
  pointer and line count are cleared.
- The partial flag (written through the caller's pointer to +0x22a) is 1 while a line is
  incomplete, which makes the next USB packet continue the same line with no code search.

Geometry, fetched from the pin by `FUN_00027a10` (vtable[3]) on attach or after a format change:
- line length = decoder output width * 2 (pin vtable +0x30 = FUN_00016944)
- pitch = `((biBitCount*biWidth)/8 + 3) & ~3` (pin +0xa0, computed by FUN_0001a928)
- lines per field = |biHeight| / 2 (pin vtable +0x38 = FUN_00016980, halved in FUN_00027a10)

### 2.5 Copy and frame completion

- `0x27920`: `dst = buffer + (field ? pitch : 0) + lines_completed * pitch * 2 + (line_length -
  bytes_left)`, then plain `memcpy`. Field 1 goes to even rows, field 2 to odd rows:
  **the two fields are woven into one frame buffer**.
- `0x279c0`: frame is done when current field index == 1, `lines_completed >= lines_per_field`
  and field1_done is set (and a buffer exists). Same as `cx231xx_is_buffer_done`.
- Both functions have a second branch for a "single field" mode (flag +0x71, lines not halved,
  second field dropped). That flag comes from pin vtable +0x40 = `0x169bc`, which is
  `xor al,al; ret` for every pin class, so the branch is dead code in this build.

### 2.6 Lost data

- A URB in which any packet has a hard error status is dropped whole: FUN_000263e0 zeroes its
  valid length and the thread skips it (10 ms of video). 50 consecutive empty/failed URBs make
  the thread query the endpoint status (FUN_000191a8, URB function 0x15) and abort the pipe
  (FUN_000190cc, function 0x02) if it reports halted; the next submit then resets the pipe
  (FUN_00018e68, function 0x1E, from FUN_00019dc0 when usb+0x48 is set).
- The parser has no explicit resync and no error flag. After a gap the current line simply keeps
  consuming bytes until its count runs out, then the scan for the next `FF 00 00 XY` resumes.
- A field that ends short leaves field1_done false (or the second field below the line count),
  so the frame is not completed; the same KS buffer is kept and overwritten by the following
  fields until a full field-1 + field-2 pair arrives. Effect: frames are silently dropped, never
  delivered partially. Same behaviour as Linux.
- If no KS buffer is available (FUN_0001e300 returns 0) data is parsed and discarded; a new
  buffer is requested at each field change.

---

## 3. Video formats, conversion, byte order

### 3.1 Advertised data ranges

Capture pin data-range pointer array at 0x2a888 (KS_DATARANGE_VIDEO, FormatSize 0x128,
specifier FORMAT_VideoInfo), decoded from .rdata:

| Addr | FOURCC | Size | bpp | AvgTimePerFrame | dwBitRate | SampleSize | Output size range | Standards mask |
|---|---|---|---|---|---|---|---|---|
| 0x30040 | UYVY | 720x480 | 16 | 333667 (29.97 fps) | 20736000 | 691200 | 80x60 .. 720x480, step 4 x 2 | 0xA03 |
| 0x302a0 | 'Y8  ' | 720x480 | 8 | 333667 | 10368000 | 345600 | same | 0xA03 |
| 0x30170 | UYVY | 720x576 | 16 | 400000 (25 fps) | 20736000 | 829440 | 88x72 .. 720x576, step 4 x 2 | 0x1FF5F0 |
| 0x303d0 | 'Y8  ' | 720x576 | 8 | 400000 | 10368000 | 414720 | same | 0x1FF5F0 |

Min and max frame interval are equal in each range (fixed rate). There is **no YUY2 range**.
Other tables with IYUV 720x480/576 (0x2d4e0, 0x2d620, 0x2db70, 0x2dcb0, referenced from
0x29358.. and 0x294f0..) belong to other filter descriptors (inferred: the software/hardware
encoder filters), not to this capture pin.

Format validation: FUN_00016ac8 checks width/height against the range's Min/MaxOutputSize and
granularity, plus biCompression and biBitCount equality; used by the intersect handler
FUN_00016d88 and SetDataFormat FUN_00016b70 (which also requires
`biSizeImage == ((biBitCount*biWidth+31)/8 & ~3) * |biHeight|`).

### 3.2 Scaling

Sizes below 720x480/576 are produced **by the chip's scaler**, not in software: FUN_000169c8
(pin RUN hook) and FUN_00016b70 call decoder vtable +0x10 = FUN_0001d4f0, which writes register
0x418 (Linux `HSCALE_CTRL`) = `((720 - w) << 20) / w` and 0x41C (`VSCALE_CTRL`) =
`-(((H - h) << 9) / h) & 0x1FFF`, H = 480 or 576. The narrower stream is also why the alt
setting depends on width (1.3).

### 3.3 No software processing

- The only operation on video payload is the `memcpy` in `0x27920`. No colour conversion, no
  byte swap, no software scaling, no deinterlacing. The delivered sample is a full frame with
  the two fields woven (field 1 on even rows).
- The KS_FRAME_INFO extended header written by `0x16f20` has `dwFrameFlags = 0`,
  ExtendedHeaderSize 0x48, PictureNumber = running counter, DropCount = pin+0x10.
- **Wire byte order is UYVY (Cb Y Cr Y)**: bytes are copied unmodified into a buffer advertised
  as UYVY. Linux reaches the same conclusion from the other side: it byte-swaps every pair
  (`cx231xx_swab`) to deliver YUYV.
- Unresolved: 'Y8  '. The copy path has no format-dependent branch and the line length is always
  width*2 (FUN_00016944), while the Y8 pitch is `width`. No code referencing the Y8 FOURCC was
  found. Either the chip is switched to an 8-bit output elsewhere (not found) or the range does
  not work; do not rely on it.

---

## 4. Audio path

- Format: the intersect handler FUN_000165e8 always answers WAVEFORMATEX PCM (tag 1),
  **2 channels, 48000 Hz, 16-bit, block align 4, 192000 bytes/s**. The KSDATARANGE_AUDIO at
  0x2a8b0 advertises 2 ch, 16 bit, 32000..96000 Hz, and the pin constructor FUN_000164a0 accepts
  32000 / 44100 / 48000 / 96000, choosing a 10 ms KS frame of 1280 / 1764 / 1920 / 3840 bytes.
  The sample rate is pushed to the hardware at ACQUIRE by FUN_000167bc -> FUN_0001ae0c.
- Transport: stream type 1 on the audio interface, alt 1, 4 URBs x 80 packets (section 1).
- Framing: for each isoc packet with Length != 0 the thread calls
  `FUN_00027d3c(stream+0x110, data, len)` (FUN_0002644c). That function appends the bytes
  verbatim to the current KS frame, completing it (FUN_0001e37c) each time it fills and carrying
  the remainder into the next one. **No header is stripped, no sample conversion** — packets are
  raw little-endian interleaved 16-bit stereo PCM. Same as Linux `cx231xx_audio_isocirq`.
- Inferred: at 48 kHz that is 24 bytes per microframe on average, within the 28-byte endpoint.
- Timestamps: same routine as video (FUN_0001e37c) — graph clock read at the moment a 10 ms
  frame fills, Duration fixed at 100000 (100 ns units).
- **A/V sync: none in the driver.** Audio and video samples are stamped independently from the
  same reference clock at completion time; there is no shared counter, no drift correction and
  no device timestamp.
- Alternative path (registry `EnableHANCAudioOut` != 0, FUN_00014048): the audio stream is
  created as type 0x81 on the HANC interface and audio arrives as ancillary packets (section 6).
  The shipped INF sets `EnableHANCAudioOut = 0` (Elgato_Video_Capture_2010.inf line 136), so the
  normal isoc audio endpoint is used.

---

## 5. Timestamps, frame drops, threads

### 5.1 Sample completion — FUN_0001e37c

Takes the pin's oldest stream-pointer clone and fills its KSSTREAM_HEADER:
- `PresentationTime.Time` = `IKsReferenceClock::GetCorrelatedTime` (clock vtable +0x28) if the
  pin has a clock, else 0; Numerator = Denominator = 1.
- `Duration` = pin+0x38: AvgTimePerFrame for video (FUN_00016848), 100000 for audio,
  166833 or 200000 for VBI (FUN_00016f60).
- `OptionsFlags` = 0x100 (DURATIONVALID), plus 0x10 (TIMEVALID) when a clock exists, plus 0x04
  (DATADISCONTINUITY) on the first sample after entering RUN (pin+0x30, set in FUN_0001e4b4).
- Then the pin's extended-header hook (vtable +0x10) and the clone is unlocked and deleted.

So the video timestamp is **the graph clock at the instant the last line of field 2 was
copied**, not a capture-start time and not derived from frame counting.

### 5.2 Buffers and drops

- Pin Process callback is `0x1e1dc` = `return STATUS_PENDING`; the driver pulls buffers itself:
  FUN_0001e300 clones the leading-edge stream pointer and advances it, only while the pin is in
  KSSTATE_RUN.
- Allocator framing (FUN_0001e240): 10 frames of biSizeImage for video, 10 frames for audio,
  8 for VBI.
- No buffer available -> incoming frame is discarded (2.6). DropCount (pin+0x10) is zeroed at
  ACQUIRE and no writer was found among the pin methods, so it stays 0 (inferred from the
  vtable's methods); PictureNumber increments only for delivered frames. Dropped frames are
  therefore visible downstream only as timestamp gaps.
- Stop/pause: FUN_0001515c detaches the pin, stops the stream (FUN_00026230: stop flag, wake and
  join thread, abort pipe, free URBs as they return), selects alt 0, and FUN_0001e45c deletes
  outstanding clones with DataUsed = 0.

### 5.3 Threads

| Creator | Thread routine | Role |
|---|---|---|
| FUN_00025c3c (stream start, one per running stream) | FUN_0002644c via thunk `0x26440` | The streaming worker. Raises itself to priority 28 (`KeSetPriorityThread`), dequeues completed URB contexts (FUN_00026ce8), parses every packet (video/VBI/HANC -> FUN_000277bc, audio -> FUN_00027d3c, TS -> FUN_0002716c), resubmits the URB (FUN_00026350), waits up to 1 s on the queue event when idle. Handles the 50-empty-URB recovery. |
| FUN_00014e34 (video pin to RUN) | FUN_000186dc | Copy-protection poll, 1 s period (section 7). Created only if `CpMethod != 0xFE`. Stopped by FUN_000187d8. |
| FUN_000124b4 (power-up, only when bcdDevice == 0) | FUN_00015944 | One-shot deferred initialisation (firmware load FUN_0001b000 etc.), then exits. Not part of streaming. |

The large functions suggested as candidates are not in the data path: FUN_000211e8 and
FUN_00022210 are AFE/decoder register programming, FUN_0001cd00 is the decoder standard/input
setup (vtable +0x08), FUN_00012dc8 is device start, FUN_00014048 constructs the objects above.

---

## 6. VBI, closed caption, HANC (identification only)

- Raw VBI: codes 0x20 / 0x60 feed the second line assembler (stream+0x178, constructed with
  1440-byte lines x 12 in FUN_00025858; copy `0x2706c`, done `0x270a8`). Each field is delivered
  as its own sample to the VBI pin (FUN_00016f60: 12 lines per field for 525-line standards,
  18 otherwise; KS_VBI_FRAME_INFO filled by FUN_00017100 with field 1/2 flags). Transport is the
  VANC interface at alt 0 through the bulk path. Linux equivalent: cx231xx-vbi.c.
- Ancillary packets: after an EAV code FUN_00027430 looks for `00 FF FF DID SDID DC`; the header
  and payload are accumulated across USB packets by FUN_00027e5c (payload = (DC & 0x0F)*4 + 1
  bytes, max 64 total) and dispatched by FUN_000276d4:
  - DID 0x91 with (SDID & 0x0F) == 6: two bytes at offsets 8..9 are written as one 2-byte sample
    to the sliced-CC pin (pin type 5).
  - DID 0xBF: embedded audio. FUN_00027ac8 byte-swaps up to eight 16-bit words and interleaves
    them into the audio pin's buffer (FUN_00027cc4); SDID bits 0/1 say which channels are
    present. Only relevant when `EnableHANCAudioOut` is set.
- Linux has no counterpart for the ancillary scanner; it ignores everything that is not an
  active-video SAV on the video endpoint.

---

## 7. Copy protection (behaviour only)

- Configuration: registry `DriverData\CpMethod`, read in FUN_00014048 (default 0xFF) and stored
  in the monitor object (dev+0x60, field +0x2c). The shipped INF sets **CpMethod = 0xFE**
  (Elgato_Video_Capture_2010.inf line 112), and FUN_00014e34 does not create the monitor thread
  for 0xFE — so on this product, as installed, the mechanism is inactive.
- Detection: FUN_0001ca4c (decoder vtable +0x68) reads decoder register 0x40C (Linux `GEN_STAT`)
  and returns a level 0..3 from bits 4..6 when bit 21 is set.
- Polling: FUN_000186dc calls it once per second. Method 4 ORs the result over three polls;
  other methods go through FUN_0001866c, which toggles the action on and off on a 5-poll cycle
  while the signal persists.
- Action: decoder vtable +0x70 = FUN_0001cac4 writes 0 to registers 0x414, 0x415, 0x420, 0x421
  (Linux: brightness, contrast, U saturation, V saturation), i.e. the picture is blanked in the
  decoder. vtable +0x78 = FUN_0001cb3c restores the cached values. The USB stream, frame
  delivery, timestamps and audio are not otherwise altered.
- `cpnotify.ax` is a user-mode component installed by the INF; nothing in the kernel data path
  references it.

---

## 8. Function table

| Windows | Role | Linux counterpart |
|---|---|---|
| FUN_00019898 | SELECT_CONFIGURATION, cache interface info | (USB core) |
| FUN_00019b30 / FUN_0001e5e0 | SELECT_INTERFACE (index+1, alt) | `usb_set_interface` in `cx231xx_set_alt_setting` |
| FUN_00015e34 | video alt by width | `cx231xx_set_video_alternate` |
| FUN_00015f48 | audio alt 1/0 | `cx231xx_set_alt_setting(INDEX_AUDIO)` |
| FUN_000161e0 / FUN_0001626c | VANC / HANC alt 0 | `cx231xx_set_alt_setting(INDEX_VANC/HANC)` |
| FUN_00016030 / FUN_00016100 | TS1 / TS2 alt | `cx231xx_set_alt_setting(INDEX_TS1/TS2)` |
| FUN_00014e34 / FUN_0001515c | per-pin stream start / stop | `cx231xx_start_streaming` / `stop_streaming` |
| FUN_00025858 | stream object ctor (URB count, EP mask) | — |
| FUN_00026714 | buffer sizing + TS_MODE_REG | `cx231xx_initialize_stream_xfer` |
| FUN_00026b6c / FUN_00026be0 | EP_MODE_SET set / clear | `cx231xx_start_stream` / `cx231xx_stop_stream` |
| FUN_00025c3c | allocate + submit URBs, create thread | `cx231xx_init_isoc` |
| FUN_00026230 | stop thread, abort pipe, free URBs | `cx231xx_uninit_isoc` |
| FUN_00018d68 / FUN_00019dc0 / FUN_00019fdc | submit isoc / bulk URB | `usb_submit_urb` |
| FUN_0001a190 / FUN_0001a224 | isoc / bulk completion routines | `cx231xx_isoc_irq_callback` |
| FUN_000263e0 | completion callback, queue URB | — |
| FUN_0002644c | stream worker thread | (work done in URB callback on Linux) |
| FUN_000190cc / FUN_000191a8 / FUN_00018e68 | abort pipe / get endpoint status / reset pipe | — |
| FUN_000277bc | packet walker, boundary + next-code search | `cx231xx_isoc_copy`, `cx231xx_find_boundary_SAV_EAV`, `cx231xx_find_next_SAV_EAV` |
| FUN_00027430 | SAV/EAV dispatch | `cx231xx_get_video_line` |
| FUN_00026f2c | line/field accounting | `cx231xx_copy_video_line` |
| FUN_00027a68 | field change, get buffer | `cx231xx_reset_video_buffer` |
| `0x27920` | copy into woven frame | `cx231xx_do_copy` (without `cx231xx_swab`) |
| `0x279c0` | frame complete test | `cx231xx_is_buffer_done` |
| FUN_00027a10 | fetch line length / pitch / height | — |
| FUN_0001e300 | get next KS buffer | `get_next_buf` |
| FUN_0001e37c | timestamp + complete KS sample | `buffer_filled` |
| FUN_0001e45c | discard outstanding buffers | `return_all_buffers` |
| FUN_00027d3c | raw audio into KS frames | `cx231xx_audio_isocirq` copy loop |
| FUN_00027e5c / FUN_000276d4 / FUN_00027ac8 / FUN_00027cc4 | ancillary packet assembly, CC, HANC audio | none |
| `0x2706c` / `0x270a8` | VBI line copy / done | `cx231xx_do_vbi_copy` / `cx231xx_is_vbi_buffer_done` |
| FUN_0002716c | TS payload copy | cx231xx-417 / dvb paths |
| FUN_00016848 / FUN_000164a0 / FUN_00016f60 | video / audio / VBI pin ctors | — |
| FUN_00016ac8 / FUN_00016d88 / FUN_00016b70 | video format check / intersect / set format | `vidioc_try_fmt` / `vidioc_s_fmt` |
| FUN_000165e8 | audio intersect (48 kHz PCM) | `cx231xx_capture` pcm hardware |
| FUN_000169c8 / FUN_0001d4f0 | program scaler for negotiated size | `cx231xx_resolution_set` |
| FUN_0001e4b4 / FUN_000167bc | pin SetDeviceState (video,VBI / audio) | — |
| FUN_000186dc / FUN_0001866c / FUN_0001ca4c / FUN_0001cac4 / FUN_0001cb3c | copy-protection monitor | none |

---

## 9. Differences from Linux

1. **Byte order**: Windows copies the wire bytes unchanged and calls the result UYVY; Linux swaps
   each byte pair and calls it YUYV. Same wire format, different delivered FOURCC.
2. **URB geometry**: Windows 80 packets x 8 URBs (video) and 80 x 4 (audio); Linux 40 x 8 and
   64 x 5.
3. **Where parsing runs**: Windows parses in a dedicated priority-28 thread fed by a queue;
   Linux parses inside the URB completion handler.
4. **Per-packet status**: Windows discards the whole URB if any packet has a hard error and
   otherwise parses every non-empty packet without looking at its status; Linux skips individual
   bad packets (except -EPROTO, which it still parses) and never discards a whole URB.
5. **Stall recovery**: Windows counts 50 consecutive bad/empty URBs, then checks endpoint status,
   aborts and resets the pipe. Linux has no equivalent.
6. **Audio mode register**: Linux writes TS_MODE_REG |= 0x9300 for the audio stream; Windows
   writes it only for HANC-audio (type 0x81) and writes nothing for the normal audio endpoint.
7. **Ancillary data**: Windows parses `00 FF FF` ancillary packets after EAV codes (sliced CC,
   embedded audio) and a packet starting with `00 FF FF`; Linux ignores them.
8. **VBI on the video stream**: the Windows walker also routes codes 0x20/0x60 to a VBI
   assembler inside the same object; Linux handles VBI only in the separate cx231xx-vbi.c path.
9. **Destination bounds check**: Linux refuses a copy that would run past the buffer
   (`cx231xx_do_copy`); the Windows copy `0x27920` has no such check and relies on the validated
   format.
10. **Scaled sizes**: both use the hardware scaler and the same width thresholds for the alt
    setting; Windows exposes the range 80x60..720x480 (88x72..720x576) in steps of 4 x 2 through
    the KS data range.
11. **Timestamps**: Windows stamps each sample with the graph reference clock at completion and
    marks only the first sample as a discontinuity; Linux uses `ktime_get_ns()` at completion.
    Neither uses a device timestamp.
12. **Copy protection**: present in Windows (disabled by the shipped INF value 0xFE), absent in
    Linux.

Same in both: no per-packet header, `FF 00 00 XY` codes with the high nibble only, 4-byte
carry-over for codes split across packets, partial-line continuation, field-change reset,
"field 2 complete and field 1 was complete" as the frame-done rule, weave of both fields into
one buffer, silent drop of incomplete frames.

---

## 10. Open points

- pcb-config index values for this board (which driver index is audio/video/VANC/HANC) were not
  read; the +1 interface mapping is read, the concrete indices are inferred from the hardware
  descriptors.
- MaximumPacketSize for the high-bandwidth alt 3 is supplied by the Windows USB stack; the
  2892-byte figure is inferred.
- 'Y8  ' ranges: no supporting code found in the copy path.
- DropCount never being incremented is inferred from the pin vtable's methods only.
