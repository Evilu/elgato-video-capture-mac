> **Appendix: working notes on the control path of `elvidcap.sys`.** Written while reading the Ghidra output, kept as
> evidence for chapters 3 and 4. Paths under `analysis/` exist only after running `./build.sh analyse` with your own
> copy of the installer. Facts settled later on the hardware: the test unit's bcdDevice is 0x4002; the Linux `cx25840`
> decoder driver was compared separately (chapter 4); composite PAL capture works with the vendor values listed here.

# elvidcap.sys (x64) control path vs. Linux cx231xx

Scope: device start-up, USB vendor requests, I2C-over-USB, register access, AFE, video decoder
block, I2S block, power modes, input mux, video standard, GPIO. Streaming/KS pin data path is
only touched where it writes control registers.

Sources: `analysis/ghidra/out/x64_elvidcap_sys/decomp.c`, the binary itself
(`analysis/unpacked/x64/elvidcap.sys`, image base 0x10000, used to read vtables and to disassemble
a few spots with `objdump -d`), `analysis/reference/linux-cx231xx/*`, and the x64 INF.

Conventions
- "FUN_xxxx" = Ghidra name = virtual address.
- **(inferred)** marks anything not read directly from code.
- `ctx` = the 0x330-byte device context allocated in FUN_00011f5c (KSDEVICE->Context, at
  KSDEVICE+0x10). Decompiled code often shows it as `longlong *param_1`, so `param_1[n]` is `ctx+8*n`.
- Ghidra renders `cx231xx_set_field(mask, v)` as a `do { mask >>= 1; v *= 2; } while(!(mask & 1))`
  loop. I evaluated those by hand: result = `v << ctz(mask)`.
- I2C device addresses in the Windows code are 7-bit (0x44, 0x30, 0x4c). Linux writes them 8-bit
  (0x88, 0x60, 0x98) and shifts right by one before use. So: **0x44 = video/audio block
  ("VID_BLK", Linux 0x88), 0x30 = AFE (Linux 0x60), 0x4c = I2S block (Linux 0x98).** Note the task
  brief had AFE/vid-block swapped; the 0x44 helpers (FUN_0001b684 etc.) are the *video block* ones.

Limits of this comparison
- The Linux reference tree here has no `cx25840-core.c`. On Linux, most of the decoder
  programming for this chip (decoder init, MODE_CTRL video format, picture controls, audio
  firmware download, I2S_IN/OUT_CTL) is done by that subdev. Where the Windows code corresponds
  to cx25840 I say so and do not claim a diff.
- The device's `bcdDevice` is not recorded anywhere in the repo. Three Windows code paths branch
  on it (==0, <0x4002, >=0x4002). See 3.2 and 4.

---

## 1. Function map

### 1.1 USB transport and vendor requests

| Windows | Linux | Evidence |
|---|---|---|
| FUN_00018f44 | (usb_control_msg core) | Sync URB submit via IofCallDriver; waits with timeout `0xfffffffffd050f80` (= 5 s), IoCancelIrp on timeout |
| FUN_000189d0 | — | Wrapper: bails if `usbdev+0xb0` (removed flag); pipe-reset handling only for URB functions 8/9/10/0x1e; then FUN_00018f44 |
| FUN_00018b10 | `__usb_control_msg` + `cx231xx_send_vendor_cmd` (non-chunked branch) | Builds 0x88-byte URB, function 0x17 (URB_FUNCTION_VENDOR_DEVICE); see 1.2 |
| FUN_0001e838 | `cx231xx_read_ctrl_reg` | bRequest 0x0d, wValue = byte-enable 1/3/7/0xf for len 1..4, wIndex = reg |
| FUN_0001e920 | `cx231xx_write_ctrl_reg` | bRequest 0x0c, same wValue/wIndex |
| FUN_00026c58 | `cx231xx_mode_register` | read reg, OR in mode, write back |
| FUN_0001e650 | `cx231xx_send_gpio_cmd` (direction=0) | bRequest 8 (or 0x0a if last arg set), wValue = val>>16, wIndex = val&0xffff |
| FUN_0001e76c | `cx231xx_send_gpio_cmd` (direction=1) | bRequest 9 (or 0x0b); **wValue/wIndex masked to 8 bits** (see 3.1) |
| FUN_0001ea60 | `cx231xx_send_usb_command` | Builds I2C wValue from dev addr, period, saddr_len, nostop, reserve; see 1.2 |
| FUN_0001fc0c / FUN_0001fcf8 | `cx231xx_read_i2c_master` / `cx231xx_write_i2c_master` | I2C master object vtable 0x2bfc0 slots +0x30 / +0x38; pack {bRequest, dev, saddr_len, saddr.., len, data} and call FUN_0001ea60 |
| FUN_0001fae0 / FUN_0001fa40 | (per-bus wrapper, `i2c_bus[n]`) | Channel wrapper vtable 0x2bf80 slots +0x30 (read) / +0x38 (write); adds channel number `obj+0x10` and takes mutex `ctx+0x2d0` |
| 0x1fbbc (via thunk 0x23a60) | chunking flags in `cx231xx_send_vendor_cmd` | Sets `regio+0x51` (nostop) / `+0x52` (reserve): (1,0)->1/0, (0,0)->1/1, (0,1)->0/1, else 0/0. Read from disassembly |
| FUN_000192bc | probe-time descriptor fetch | URB 0x0b GET_DESCRIPTOR type 1 len 0x12 into `usbdev+0x10`; then FUN_0001961c (config descriptors), FUN_00019898 (select configuration) |
| FUN_00019898 | (usb_set_configuration) | USBD_CreateConfigurationRequestEx |
| FUN_00019b30 / FUN_0001e5e0 | `cx231xx_set_alt_setting` | Select-interface for (interface number, alt); FUN_0001e5e0 is the locked wrapper |
| FUN_00015e34 | `cx231xx_set_video_alternate` | width > 360 -> alt 3, > 180 -> alt 2, > 0 -> alt 1 (constants 0x169, 0xb5); interface from pcb config `+0x1a + 9*cfg` |
| FUN_00015f48 / 16030 / 16100 / 161e0 / 1626c | `cx231xx_set_alt_setting` for audio / TS1 / TS2 / VANC / HANC | Interface bytes at pcb config +0x19, +0x17, +0x18, +0x1b, +0x1c (+9 per config) |
| FUN_0001fdc0 / FUN_0001ff84 | `initialize_cx231xx` (cx231xx-pcb-cfg.c) | Read reg 0x00 (BOARD_CFG_STAT) 4 bytes; ff84 switches on `&0x86` = 4/6/0x80/0x82/0x84/0x86 (self power) and `&0xc4` = 0x44/0xc0/0xc4 (bus power), copies a 0x4c-byte scenario; fdc0 is a different 17-entry table used only when bcdDevice == 0 |

### 1.2 How the vendor request is built (FUN_00018b10)

Request descriptor passed in (`param_2`): `+0` bRequest, `+2` wValue, `+4` wIndex, `+6` wLength,
`+8` direction (1 = IN), `+0x10` buffer. URB fields: `+0x02` = 0x17, `+0x20` TransferFlags = 1
if IN else 0, `+0x24` length, `+0x28` buffer, `+0x80` RequestTypeReservedBits = 0, `+0x81`
Request, `+0x82` Value, `+0x84` Index. So bmRequestType is 0x40 / 0xC0 (vendor, device), same
as Linux.

- Retries: loops while the status is negative, up to 4 attempts, unless `usbdev+0xd8 == 0x1a`
  (that field is the `TunerType` registry value; 0x28 on this device, so retries are active).
  Linux has no retry.
- Register read/write (FUN_0001e838/e920): identical to Linux. Lengths other than 1..4 are
  rejected with 0xC000000D.
- I2C (FUN_0001ea60), when bcdDevice != 0:
  `wValue = dev7 << 9 | reserve << 6 | period << 4 | saddr_len << 2 | nostop << 1 | 1`,
  with `period = regio+0x50`, `nostop = regio+0x51`, `reserve = regio+0x52`. This is the Linux
  formula bit for bit. bRequest = channel for write, channel + 4 for read. wIndex = 0 /
  saddr byte / 16-bit saddr for saddr_len 0/1/2. wLength = data length.
  When bcdDevice == 0 the formula drops reserve and forces nostop=1 (`... | 3`); legacy path.
- **4-byte chunking**: the Windows primitive does *not* chunk. Linux `cx231xx_send_vendor_cmd`
  splits any I2C read (bRequest 4/5/6) or master-2 write longer than 4 bytes into 4-byte pieces
  with wValue flags `|0x02`, `|0x42`, `|0x40`. On Windows the same three flag states exist
  (0x1fbbc) but are set explicitly by the one caller that needs them, the Merlin firmware
  loader FUN_0001b92c, which sends the file 4 bytes at a time. Every register helper on this
  board transfers 1 or 4 bytes, so chunking never triggers in either driver.
- Data byte order for 32-bit registers is little-endian in both (FUN_0001b840).

Concrete wValue values with the shipped INF (`I2CSpeed` = 1): video block 0x8819 (4- and 1-byte,
saddr_len 2), AFE 0x6019, I2S 0x9815. Linux uses period 2: 0x8829 / 0x6029 / 0x9825.

### 1.3 Register helpers

| Windows | Linux | Evidence |
|---|---|---|
| FUN_0001b550 | — | Takes the vid-block mutex (`obj+8`) |
| FUN_0001b684 | `vid_blk_read_byte` | dev 0x44, saddr_len 2, len 1 |
| FUN_0001b700 | `vid_blk_write_byte` | dev 0x44, saddr_len 2, len 1 |
| FUN_0001b8b0 | `vid_blk_read_word` | dev 0x44, saddr_len 2, len 4 |
| FUN_0001b788 (locked) / FUN_0001b840 (raw) | `vid_blk_write_word` | dev 0x44, saddr_len 2, len 4, LE bytes |
| FUN_0001b7dc | `cx231xx_read_modify_write_i2c_dword` | read, `(v & ~mask) | value`, write |
| FUN_0001bce0 | `cx231xx_reg_mask_write` (size 32) | args (reg, start_bit, end_bit, value); used with 0x104 bits 30-31, 23-24, 15-22, 9 |
| FUN_0002249c | `cx231xx_reg_mask_write` (size 8) | same on AFE; used with 0x2a/0x4a/0x6a bits 3-7 |
| FUN_0002255c / FUN_000225f4 | `afe_read_byte` / `afe_write_byte` | dev 0x30, saddr_len 2, len 1 |
| FUN_00022de0 / FUN_00022e70 | `cx231xx_read_i2c_data` / `write_i2c_data` on I2S_BLK | dev 0x4c, saddr_len 1, len 1 |
| FUN_0001b578 | `stopAudioFirmware` (stateful) | if state == 0: write 0x800 = 0x03000000, state = 2 |
| FUN_0001b620 | `restartAudioFirmware` (stateful) | if state == 2: write 0x800 = 0x13000000, state = 0 |
| FUN_0001a54c / FUN_0001a5b0 | — | KS property handlers: raw VRT_GET/SET_REGISTER passthrough |
| FUN_0001a7cc / FUN_0001a83c | — | KS property handlers: raw GPIO get/set passthrough |

### 1.4 AFE (I2C 0x30)

| Windows | Linux | Evidence |
|---|---|---|
| FUN_00022688 | `cx231xx_afe_init_super_block` | 0x01 = ref&0xff, 0x00 = (ref>>8 & 3)|0x40, 0x05 = 0x0f, loop 0x08 = 0x18 until readback 0x18, 0x02 = 0x40, 5 ms, 0x02 = 0. Plus a Windows-only tail (3.2) |
| FUN_000228b4 | `cx231xx_afe_init_channels` | 0x23/43/63 = 0; 0x0b = 2; 0x27/47/67 = 0x17; 0x22/42/62 = 0x10; 5 ms; 0x27/47/67 = 7; 0x29/49/69 = 0xf0; mask-write 0x2a/4a/6a bits 3-7 = 0; 0x26/46/66 = 3. Identical |
| FUN_00022d40 | `cx231xx_afe_adjust_ref_count` | reads 0x68+0x69 (input type 1 = tuner) or 0x28+0x29; `(ntf&3) | ((in&6)<<1)`; different ref counts (3.2) |
| FUN_00022ad0 | `cx231xx_afe_set_mode(AFE_MODE_BASEBAND)` | 0x43 &= ~0x50; re-adjust ref count only if previous mode != 1 and input type == 1 |
| FUN_00022b38 | `cx231xx_afe_update_power_control` | switch on mode 0x10/0x20/0x30 and on BoardType; 0x08 = 0x18 loop; writes 0x23/0x43/0x63 |
| inline in FUN_0001cd00 | `cx231xx_afe_set_input_mux` | regs 0x28/0x48/0x68 from the three bytes of the mux value (3.2) |

### 1.5 Video decoder block (I2C 0x44) and input selection

Decoder object: 0x68 bytes, built by FUN_0001bf48, vtable 0x2bdb0. Fields: `+0x08` KS standard
bitmask, `+0x0c` internal standard code, `+0x10..+0x20` picture controls, `+0x24` mux, `+0x28`
input type, `+0x30` DIF object, `+0x38` ctx, `+0x40` vid-block object, `+0x48` AFE object,
`+0x50/+0x54` width/height, `+0x60` TunerType.

| Windows | Linux | Evidence |
|---|---|---|
| FUN_00012b08 | `cx231xx_set_video_input_mux` + `cx231xx_set_audio_input` (dispatch on pin type) | For video types: set power mode 0x30 if not already, then decoder vtable+8. For type 4: I2S 0x0f = 0, 0x0e = 0x80, then audio decoder input 1 |
| FUN_0001cd00 (vtable +0x08) | `cx231xx_set_decoder_video_input` | adjust ref count if type changed; AFE mux; per type: AFE_CTRL 0x104, OUT_CTRL1 0x404, DIF baseband, DFE_CTRL1 0x488 |= 0x18000000, MODE_CTRL 0x400 ACFG_DIS / INPUT_MODE; tail 0x404 bit 3 = 1 and `if (0x404 & 2) |= 0x80000`; then FUN_0001d90c |
| FUN_0001d90c | `cx231xx_do_mode_ctrl_overrides` | 100 ms delay; 0x490 = 0xcd3f0280; 0x474 / 0x470 fields (values differ, 3.3) |
| FUN_0001d478 (vtable +0x18) | cx25840 `set_v4lstd` **(inferred, source not in tree)** | dispatch on standard code to FUN_0001c318 / c430 / c598 / c738, then FUN_0001d90c |
| FUN_0001c0d8 | cx25840 decoder init **(inferred)** | common prologue of every standard change; listed in 2.3 |
| FUN_0001c318 / c430 / c598 / c738 | cx25840 standard setup **(inferred)** | MODE_CTRL low nibble = 1 (NTSC-M), 5 (PAL-M), 8 (PAL-60) / 4 (PAL) / 0xc (SECAM) / 7 plus bit 6 (PAL-N) |
| FUN_00011b34 | (s_std entry) | maps KS bitmask to code: `&0xa03` -> 1, `&0xff000` -> 2, 0x400 -> 4, 0x100000 -> 5, else 0; calls vtable+0x18, FUN_000211e8(std), vtable+8 |
| FUN_0001d4f0 (vtable +0x10) | cx25840 scaler setup **(inferred)** | 0x418 = ((720-w)<<20)/w & 0xffffff; 0x41c = -(((lines-h)<<9)/h) & 0x1fff; lines = 480 for codes 1/6 else 576 |
| FUN_0001d598 / d60c / d684 / d6f0 / d77c | brightness / contrast / hue / saturation / sharpness | byte regs 0x414, 0x415, 0x422, 0x420+0x421, 0x416[2:0] |
| FUN_0001cc50 | — (no Linux equivalent found in tree) | 0x414 bits 21:20 = 1, 0x420 bits 25:24 = 1, 0x478 bits 21:20 = 2, 0x478 bits 23:22 = 0 |
| FUN_0001cbc4 (vtable +0x98) | — | re-applies standard, scale, input, picture controls (used on resume, FUN_000122a8) |
| FUN_0001d868 (vtable +0x28) | `cx231xx_enable656` minus the read | byte 0x07 = 0xff, byte 0x08 = 0x0f |
| FUN_0001c9e8 / FUN_0001c7f8 | — | 0x404 |= 0x80 / 0x404 &= ~0x80 plus MODE_CTRL bits; vtable slots +0xb0 / +0xa8, callers not in the control path |
| FUN_0001ca4c, FUN_0001da10 | — | read GEN_STAT 0x40c: detected format / lock status |
| FUN_00012440 | `cx231xx_init_ctrl_pin_status` | 0x120 |= ~ctrl_pin_status_mask |

### 1.6 Audio / Merlin (I2C 0x44, registers 0x800+)

| Windows | Linux | Evidence |
|---|---|---|
| FUN_0001a978 | `cx231xx_set_audio_decoder_input` (given) | 0x810 bit 0 set/clear around the switch; mode 0 = mute (0x8d0 = 0x1f011012), 1 = line, others = tuner |
| FUN_0001ae0c | — (not in tree) | sample-rate SRC regs: 48 kHz -> 0x8f8 = 0x8fc = 0x0801867c, 0x900 = 0x904 = 0x90c = 0x08014faa |
| FUN_0001b000 | cx25840 audio init + firmware load **(inferred)** | optional merlinD.rom, then 0x914 = 0xa0, 0x918 = 0x1a0, 0x808 = 0xf6, FUN_0001b254, FUN_0001a978 |
| FUN_0001b92c | cx25840 firmware download **(inferred)** | 0x803 = 0x0b; reg 0x000 |= 0x20; 0x801 = 0; 0x800 = 0; file data 4 bytes per I2C write with nostop/reserve chunk flags, trailing bytes via byte reg 0x802; reg 0x000 &= ~0x20; 0x800 = (v & 0xffffff) | 0x03000000 |
| FUN_0001b254 | — | reads firmware version bytes 0x807 / 0x806 into `audio+0x1c` after a 0x800 / 0x808 save-restore dance and 50 ms sleep |
| FUN_0001aef0, FUN_0001b184, FUN_0001b460, FUN_0001b3ac | — | tuner audio standard (0x808, 0x809), FM setup (0x9c4, 0x81c, 0x824, 0x8cc, 0x818), status poll 0x804. Tuner only |
| I2S init inline in FUN_00014048 | `cx231xx_i2s_blk_initialize` | 0x0e |= 0x80; 0x0f = 0 |
| I2S tail of FUN_000154c0 | `cx231xx_i2s_blk_update_power_control` | mode != 0x30: 0x0f |= 0xfe; else 0x0f = 0 |
| I2S in FUN_00012b08 (type 4) | `cx231xx_i2s_blk_set_audio_input` | 0x0f = 0, 0x0e = 0x80 |

### 1.7 Power, GPIO, streams, tuner

| Windows | Linux | Evidence |
|---|---|---|
| FUN_000154c0 | `cx231xx_set_power_mode` | reg 0x74 (PWR_CTL_EN); modes 0x10 / 0x20 / 0x30; then AFE and I2S power control |
| FUN_000153c4 | part of `cx231xx_dev_init` | optional reset pulse on 0x74, then power mode 0x20 |
| FUN_00016358 | `cx231xx_enable_i2c_port_3` | toggles bit 0 of reg 0x74 (arg 1 = clear) |
| FUN_00012148 | (suspend) | D3/D4: 0x74 &= 0xfffffe80, marks USB device unconfigured |
| FUN_000124b4 / FUN_000122a8 | (resume) | D0: re-fetch descriptors, power mode, AFE re-init, decoder re-apply, audio re-init |
| FUN_00022f50 | `cx231xx_set_gpio_direction` | GPIO object vtable 0x2c170 slot +0x00; dir at `gpio+0x18`, value at `gpio+0x1c` |
| FUN_00022fe4 | `cx231xx_set_gpio_value` | slot +0x08; forces pin to output first, then value |
| FUN_00023900 / FUN_00023978 | `cx231xx_set_gpio_bit` / `cx231xx_get_gpio_bit` as used by the GPIO bit-bang | dir masked with 0xfc0003ff |
| FUN_00010308 | `initGPIO` | dir 0x03fdfc00, Verve reg 7 = 0xf4. Only when the cx23416 object exists (`Enable_SW_Encoder` = 0) |
| FUN_00010540, FUN_000105c8 .. FUN_000119d0 | cx231xx-417.c | GPIO 9 reset pulse; mc417 access through GPIO. Not created on this board |
| FUN_00026714 | `cx231xx_initialize_stream_xfer` | reg 0x04 |= 0x100 / 0x300 / 0x1300 / 0x9300 by media type; TS1: 0x04 / 0x08 / 0x0c; full speed: 0x04 |= 0x101 |
| FUN_00026b6c / FUN_00026be0 | `cx231xx_start_stream` / `cx231xx_stop_stream` | reg 0x18 (EP_MODE_SET) |= / &= ~ `stream+0x3e8` |
| FUN_000211e8 | `cx231xx_dif_set_standard` | reads 0x33c, per-standard tables over 0x304..0x394; unused for baseband except the bypass branch |
| FUN_00022210 | `cx231xx_dif_configure_C2HH_for_low_IF` | mask-writes on 0x104 bits 30-31 / 23-24 / 15-22 / 9, and 0x124 = 3 |
| FUN_0001f55c, FUN_0001f4a4, FUN_0001ed84 .. FUN_0001f328 | tuner / channel tuning | country and channel tables at 0x2aa30 / 0x2aa90. Unused: no tuner object is ever stored at `ctx+0x70` (3.6) |
| FUN_000239f4, FUN_00023a88 .. FUN_0002425c | `cx231xx_gpio_i2c_*` and tuner/demod I2C | bit-bang I2C through GPIO; tuner only |
| FUN_00020474, FUN_00020628, FUN_000209dc, FUN_00020a88 | BDA (DVB) filter | only when `Enable_BDA` = 1 (INF sets 0) |

---

## 2. Start-up sequence

### 2.1 Entry and dispatch

- Driver entry FUN_00031ee4 -> FUN_000211d4 -> `KsInitializeDriver(.., &0x296f8)`.
- KSDEVICE_DESCRIPTOR at 0x296f8 points at KSDEVICE_DISPATCH 0x2b9e0 (read from the binary):
  Add = FUN_00011f5c, Start = FUN_00011fa8, Stop = 0x11fdc, Remove = FUN_000120b4,
  SurpriseRemoval = FUN_00011fec, QueryPower = 0x12108, SetPower = FUN_0001267c.
- The INF binds the driver to `USB\VID_0FD9&PID_0037&MI_01` (interface 1 of a composite device;
  MI_00 is the IR function). Vendor requests are still device-recipient.

### 2.2 Call tree for PnP start

```
FUN_00011f5c  Add: allocate ctx (0x330), FUN_00011c18 constructor
              defaults: ctx+0x2c4 power mode = 0, ctx+0x2c8 gpio mask = 0xffffffff,
              ctx+0x2c3 = 1, ctx+0x1a4 = 0xff, ctx+0x31c = 0xffffffff
FUN_00011fa8  Start
└─ FUN_00012dc8
   ├─ allocate USB device object -> ctx+0x28; reads "TunerType" into usbdev+0xd8
   ├─ FUN_00018934 -> FUN_000192bc
   │    ├─ GET_DESCRIPTOR(device, 0x12)            -> usbdev+0x10 (bcdDevice at usbdev+0x1c)
   │    ├─ FUN_0001961c  GET_DESCRIPTOR(config) for each configuration
   │    └─ FUN_00019898  select configuration (index usbdev+0x30)
   ├─ registry: Enable_SW_Encoder, Enable_BDA, NoOfTuners
   ├─ FUN_00019460  read serial-number string descriptor (only if bcdDevice != 0)
   ├─ allocate register/vendor-request object -> ctx+0x30; registry I2CSpeed -> regio+0x50
   ├─ registry BoardType (default 0x0c) -> ctx+0x2a4
   ├─ allocate pcb-config object -> ctx+0x08
   ├─ FUN_0001ff84  read reg 0x00 (BOARD_CFG_STAT), pick scenario   [bcdDevice != 0]
   │   (FUN_0001fdc0 instead if bcdDevice == 0)
   ├─ registry TunerI2CMaster, DemodI2CMaster
   ├─ FUN_000153c4                                  [skipped entirely if bcdDevice == 0]
   │    ├─ if pcb type byte == 0 (self powered): reg 0x74 = 0x00000000, 5 ms,
   │    │                                         reg 0x74 = 0x00000100, 5 ms
   │    └─ FUN_000154c0(ctx, 0x20)   power mode ANALOGT_TV; AFE and I2S objects do not
   │                                 exist yet, so only reg 0x74 is touched
   ├─ registry BDA_Demod_Tuner_Type, TunerI2CAddress, DemodI2CAddress
   ├─ FUN_00014048  hardware object construction and init (2.3)
   ├─ FUN_00013564  KsCreateFilterFactory x N (no hardware access)
   ├─ FUN_000138c4  device-interface registry bookkeeping (only if bcdDevice != 0)
   ├─ registry TunerType -> ctx+0x2a0
   ├─ AGC GPIO: bit = gpio->vtable[+0x30]() (= byte gpio+0x15),
   │            gpio->vtable[+0x08](bit, BoardType is 9 or 10 ? 0 : 1)   = FUN_00022fe4
   ├─ ctx+0x70 tuner object: null, skipped
   ├─ ctx+0x298 cx23416 object: null because Enable_SW_Encoder = 1, FUN_00010308 skipped
   ├─ KsRegisterFilterWithNoKSPins
   ├─ FUN_000179fc  crossbar object -> ctx+0x68 (2.4)
   ├─ FUN_00018104  crossbar filter registration
   ├─ FUN_0001b000  audio init (2.5)
   └─ IoSetDeviceInterfaceState(TRUE); ctx+0xf0 = 1
   any failure -> FUN_00013b54 (teardown)
```

### 2.3 FUN_00014048 in order

1. Registry: demod_reset_gpio_bit, if_select_gpio_bit, tuner_i2c_enable_gpio_bit (defaults 0xff),
   gpio_pin_status_mask -> ctx+0x2c8, XC_GPIO_I2C_SCL / SDA (defaults 0).
2. GPIO object -> ctx+0x18 (dir = 0, value = 0 cached).
3. FUN_00012378: walks bits 0..31 and drives every pin whose mask bit is 0 low as an output,
   but stops at the first bit equal to SCL, SDA or tuner_reset. With the shipped INF this
   returns at bit 0 without any USB traffic (3.5).
4. BoardType 3/6/7 only: tuner-reset GPIO object, FUN_000163e8.
5. Registry AGC_Analog_Digitial_Select_Gpio_Bit: if present, stored via gpio vtable +0x38
   (FUN_00022f2c -> byte gpio+0x15). No USB traffic here.
6. I2C master object -> ctx+0x198; three channel wrappers (channel 0/1/2) -> ctx+0x78/0x80/0x88;
   tuner and demod I2C objects -> ctx+0x90/0x98 (no traffic).
7. Registry VideoStandard (default 1), DisableVBI, DisableAudioOutPin, EnableHANCAudioOut.
8. Vid-block object -> ctx+0x148 on channel 0: **write 0x800 = 0x03000000** (FUN_0001b840), then
   read 1 byte at reg 0x100 and keep `& 0xf` at `obj+0x44`. Audio-firmware state = 1.
9. Second vid-block-style object -> ctx+0x150; DIF object -> ctx+0x10 (mode 1).
10. AFE object -> ctx+0xa0 (ref count 0x210, input type 2, mode 1):
    FUN_00022688(0x210), then FUN_000228b4.
11. I2S object -> ctx+0xa8: read 0x0e, write 0x0e |= 0x80, write 0x0f = 0.
12. Decoder object -> ctx+0x50, FUN_0001bf48(std = VideoStandard), input type 2, mux 2:
    - FUN_00011b34(std): vtable+0x18 = FUN_0001d478(code) -> for NTSC FUN_0001c318:
      - FUN_0001c0d8: 0x138 bit 0 pulse (set, clear); 0x4a4 = 0x8000 then 0; restore picture
        controls (FUN_0001cb3c: 0x414, 0x420, 0x421, 0x415); 0x498 = (v & 0xfff00802) | 0x802;
        0x400 bit 5 = 1; 0x400 &= ~0x1800; 0x494 = (v & 0xffff1000) | 0x1000; 0x420 bit 29 = 0;
        0x404 bits 1:0 = 2; 0x404 bit 3 = 1; **FUN_0001cd00(type, mux)**; 0x400 |= 0x2000;
        0x33c = 0x80000000; if (0x404 & 2) 0x404 |= 0x80000.
      - 0x400 bit 5 = 0; 0x400 low nibble = 1 (5 for PAL-M, 8 for PAL-60); 0x400 bit 5 = 1;
        0x478 bit 31 = 0; 0x42c = 0x66000000.
      - FUN_0001d90c.
    - FUN_000211e8(dif, std) (DIF programmed for the real standard, 3.3), then FUN_0001cd00 again.
    - Picture controls: 0x414 = 0x00, 0x415 = 0x80, 0x416[2:0] = 5, 0x420 = 0x421 = 0x80,
      0x422 = 0x00 (from defaults 5000/5000/50/5000/5000 at 0x2df18, 0x2df1c, 0x2e748, 0x2e05c,
      0x2e058).
    - FUN_0001d4f0(720, 480): 0x418 = 0, 0x41c = 0.
    - FUN_0001cd00(2, 2) a third time; FUN_0001cc50.
13. Audio decoder object -> ctx+0x58 (input 1 = line, 48000 Hz). No traffic.
14. `Enable_SW_Encoder` = 0 only: cx23416 object -> ctx+0x298, FUN_00010540. Skipped here.
15. Registry ctrl_pin_status_mask -> ctx+0x2cc; FUN_00012440: 0x120 |= ~mask (= |= 0x3b).
16. Registry CpMethod -> copy-protection object ctx+0x60 (`+0x2c`); TS1_Source -> pcb config +4.
17. Stream objects (FUN_00025778) per pcb config; no register traffic.

FUN_0001cd00(type 2 = composite, mux 0x02), as executed each time:
```
[if type changed] FUN_00022d40 -> FUN_00022688(ref)
AFE 0x28: read, then write (ch1-1)<<4 = 0x10          (ch2, ch3 bytes are 0: skipped)
0x104 = (v & 0xff007fdf) | 0x01000010
0x404 |= 0x80
0x404 bits 1:0 = 2
FUN_000211e8(dif, 0xffffffff): read 0x33c; 0x340 = 0x0df7df83; 0x33c |= 0x80000000;
                               0x33c = v & ~0x00080000
0x488 |= 0x18000000
0x400 bit 5 = 1
0x400 bits 10:9 = 0
0x404 bit 3 = 1
if (0x404 & 2) 0x404 |= 0x80000
FUN_0001d90c: sleep 100 ms; 0x490 = 0xcd3f0280; 0x474 / 0x470 per standard
```
The AFE_CTRL / DIF / DFE_CTRL1 block is conditional on `dec+0x30` (DIF object) being non-null;
FUN_00014048 always passes one, so it always runs.

### 2.4 Crossbar constructor FUN_000179fc

Reads VideoInputPin (0) and AudioInputPin (2), builds the pin list from `DriverData\XBarPinN`
(FUN_00017be0), reads ForceAudioWithVideoPin (1), then routes (all guarded by `ctx+0x210 == 0`):

```
FUN_00017f10(xbar, pin 0, 0)
├─ FUN_00012b08(ctx, 8, 0)          mute: FUN_0001a978(audio, 0)
├─ FUN_00012b08(ctx, 2, 0x02)       composite
│    ├─ FUN_000154c0(ctx, 0x30)     because current mode is 0x20
│    │    reg 0x74: read; write (v & 0xfffffe88) | 0x08; 5 ms; write .. | 0x48; 5 ms;
│    │              write .. | 0x78; 5 ms
│    │    FUN_00022b38(afe, 0x30)   BoardType 10: loop 0x08 = 0x18; 0x23 = 0; 0x43 = 0; 0x63 = 0
│    │    I2S 0x0f = 0
│    │    reg 0x74 read back
│    └─ FUN_0001cd00(dec, 2, 0x02)
└─ FUN_00012b08(ctx, 4, 0)          audio line: I2S 0x0f = 0, 0x0e = 0x80; FUN_0001a978(audio, 1)
FUN_00012b08(ctx, 4, 0)             again, from the tail of FUN_000179fc
```

FUN_0001a978(audio, 1) = line in: FUN_0001ae0c(48000) (SRC regs); 0x810 |= 1; FUN_0001b578
(no write unless firmware state is 0); 0x124 = 0; 0x91c |= 0x01000000; 0x910 = 0;
0x8d0 = 0x00063073; 0x8d4 |= 0x7fff0000; 0x8dc |= 0x7fff0000; 0x810 &= ~1.

### 2.5 FUN_0001b000 (last hardware step of start)

1. ZwCreateFile `\systemroot\system32\drivers\merlinD.rom`; only if it opens:
   FUN_0001b92c(vidblk, handle, bcdDevice).
2. 0x914 = 0xa0; 0x918 = 0x1a0; 0x808 = 0xf6 (each must succeed to continue).
3. FUN_0001b254 (version read; result only stored).
4. FUN_0001a978(audio, current input) -> line in again.

---

## 3. Differences from Linux for the Elgato V2 board

### 3.1 Transport

| Item | Windows | Linux | Where |
|---|---|---|---|
| I2C period bits (wValue bits 5:4) | 1 (INF `I2CSpeed` = 1; default 0 if value missing) | 2 (`I2C_SPEED_100K`) on all three buses | FUN_00012dc8, FUN_0001ea60 / `cx231xx_dev_init` |
| Retry | up to 4 attempts per vendor request | none | FUN_00018b10 |
| Timeout | 5 s | HZ (1 s) | FUN_00018f44 |
| >4-byte chunking | only in firmware loader | generic in `cx231xx_send_vendor_cmd` | FUN_0001b92c / 0x1fbbc |
| GET_GPIO / GET_GPIE wValue, wIndex | masked to 8 bits each (`and cx` with 0xff at 0x1e7d4 / 0x1e7df, confirmed in disassembly) | 16 bits each | FUN_0001e76c. Only reached from the GPIO bit-bang code (FUN_00023978), which this board never runs |

### 3.2 Power and AFE

| Item | Windows | Linux | Where |
|---|---|---|---|
| First power mode | 0x20 (ANALOGT_TV), unconditionally; switches to 0x30 at the first composite/S-Video route | 0x30 directly because `.external_av = 1` | FUN_000153c4 / `cx231xx_dev_init` |
| Mode 0x20 sequence on reg 0x74 | set bit 3, set bit 6, AV_MODE = 0x20, then **set bit 0 and clear bit 1**, then set bit 2 | set bit 1 first, then bits 2, 3, 6, then 0x20; bit 0 untouched | FUN_000154c0 |
| Mode 0x30 values | 0x08, 0x48, 0x78 over `v & 0xfffffe88` | same three values over `v & ~0x17f` | same result |
| Delay between 0x74 writes | 5 ms | 10 ms (`PWR_SLEEP_INTERVAL`) | FUN_000154c0 |
| Early exit when mode unchanged | none (callers check `ctx+0x2c4`) | returns 0 | |
| ADC_PWRDN_CLAMP_CH3 (AFE 0x63) in mode 0x30 | **0x00** (BoardType 3 or 6..11 takes the "all channels on" branch) | **0x40** (Elgato V2 is not in the model list, so the default branch powers CH3 down) | FUN_00022b38 / `cx231xx_afe_update_power_control` |
| Super-block ref count at init | **0x210** (TUNE2 0x01 = 0x10, TUNE1 0x00 = 0x42) | **0x23c** (TUNE2 = 0x3c, TUNE1 = 0x42) | FUN_00014048 / `cx231xx_dev_init` |
| adjust_ref_count table | input_mode 0 or 4 -> 0x210; 9 or 0xa -> 0x225 | 0 -> 0x23c, 4 -> 0x24c, 9 -> 0x258, 0xa -> 0x260 | FUN_00022d40 |
| Super-block loop | no iteration cap, no TUNE2 readback | cap of 10, reads TUNE2 back | FUN_00022688 |
| Super-block tail, bcdDevice < 0x4002 (and unknown = 0xff) | extra write AFE 0x0a = 0x72 | nothing | FUN_00022688 |
| Super-block tail, bcdDevice >= 0x4002 | 0x0a = 0x52; 0x28 in_sel incremented; 10 ms; 0x21 = 1; 50 ms; 0x21 = 0; 50 ms; read 0x21, if bit 1 then 0x0a = 0x72; restore 0x28; 0x27 = (v & 0xfd) | 1; **0x00 = 0xbc | (ref>>8 & 3)** | nothing | FUN_00022688 |
| AFE input mux write | **overwrites** the register: 0x28/0x48/0x68 = (n-1)<<4 (read value discarded; confirmed at 0x1cd6f..0x1cd8a) | read-modify-write of bits 5:4 only | FUN_0001cd00 / `cx231xx_afe_set_input_mux` |
| S-Video mux value | **0x0301**: 0x28 = 0x00, 0x48 = 0x20 (INF XBarPin1 InputMux = 01 03 00 00) | 0x0101 (`VIN_1_1 | VIN_1_2 << 8`): CH2 in_sel = 0 | INF / cx231xx-cards.c |
| Composite mux value | 0x02 -> 0x28 = 0x10 | `CX231XX_VIN_2_1` = 2, same input | no difference in selection |
| init_channels | identical | | FUN_000228b4 |

### 3.3 Video decoder block

| Item | Windows | Linux | Where |
|---|---|---|---|
| VERT_TIM_CTRL 0x474, NTSC / PAL-M: VACTIVE_CNT | **0x1e6** (field value 0x1e6000) | 0x1e7 (0x1e7000) | FUN_0001d90c / `cx231xx_do_mode_ctrl_overrides` |
| same, V656BLANK_CNT | **0x1e** (0x1e000000) | 0x1c (0x1c000000) | |
| same, VBLANK_CNT; HORIZ_TIM_CTRL 0x470 HBLANK_CNT | 0x18; 0x79 | 0x18; 0x79 | same |
| 0x474, PAL / SECAM / PAL-N: VBLANK_CNT | **0x24** | 0x20 | |
| same, VACTIVE_CNT and V656BLANK_CNT | **not written** | 0x244 and 0x24 | |
| same, HBLANK_CNT | 0x85 | 0x85 | same |
| PAL-60 (code 6) | only 0x490 written | treated as non-NTSC branch | |
| Delay before overrides | 100 ms | none | FUN_0001d90c |
| DFE_CTRL3 0x490 | 0xcd3f0280 | 0xcd3f0280 | same |
| Composite: AFE_CTRL, OUT_CTRL1, DFE_CTRL1, MODE_CTRL, tail | no value differences found | | FUN_0001cd00 |
| S-Video: AFE_CTRL = (v & 0xff007fff) | 0x01000010; INPUT_MODE = 1; AFE_CTRL = (v & ~0x180) | 0x1000; AFE 0x43 &= ~0x50 | no value differences found | | FUN_0001cd00, FUN_00022ad0 |
| DIF baseband branch | 0x340 = 0x0df7df83, 0x33c |= 0x80000000, then clear bit 19 | identical | FUN_000211e8 |
| DIF on standard change | **also runs the full tuner-standard path** each time the standard is set (FUN_00011b34): FUN_00022210 writes 0x104 bits 31:30 = 1, bits 24:23 = 1, bits 22:15 = 0x0b (M/N), 0x0e or 0x0f (others), bit 9 = 1, and for M/N 0x124 = 3; then the 0x304..0x344 table and 0x33c. Input select follows and re-enters baseband | only `DIF_USE_BASEBAND` is ever requested for composite / S-Video | FUN_00011b34, FUN_000211e8, FUN_00022210 |
| Consequence for 0x104 | bits 30 and 9 stay set after input select, because the composite mask 0xff007fdf preserves them **(inferred from the masks; not observed on hardware)** | left at whatever reset / cx25840 leaves | |
| Extra write in standard prologue | 0x33c = 0x80000000 written outright | — | FUN_0001c0d8 |
| Decoder init, format select, scaler, picture controls | done in this driver (2.3 step 12) | done by cx25840, not in the reference tree: **not compared** | |

### 3.4 Audio

| Item | Windows | Linux | Where |
|---|---|---|---|
| Line in: DL_CTL 0x800 | not written in the line branch. 0x03000000 is written once at object creation (FUN_00014048) and by FUN_0001b578 only if the firmware was running | writes 0x03000001 in the line branch | FUN_0001a978 |
| Line in: 0x124, 0x91c, 0x910, 0x8d0, 0x8d4, 0x8dc | 0, |= 0x01000000, 0, 0x00063073, |= 0x7fff0000, |= 0x7fff0000 | same values | same |
| SRC registers before every input switch | 0x8f8 / 0x8fc / 0x900 / 0x904 / 0x90c for 48 kHz (1.6) | not in avcore | FUN_0001ae0c |
| 0x914 = 0xa0, 0x918 = 0x1a0, 0x808 = 0xf6 | written at start and on resume | not in avcore (cx25840 territory, not compared) | FUN_0001b000 |
| I2S block init / power / line select | identical values | | FUN_00014048, FUN_000154c0, FUN_00012b08 |
| TS_MODE_REG 0x04 for the audio stream | media type 1 (EnableHANCAudioOut = 0, as shipped) has **no case**: nothing written. 0x9300 only for type 0x81 | 0x9300 for Audio | FUN_00026714. Streaming path; flagged for whoever owns it |

### 3.5 GPIO and pin masks

- `ctrl_pin_status_mask` 0xFFFFFFC4: same value and same use in both (PIN_CTRL 0x120 |= 0x3b).
  FUN_00012440 / `cx231xx_init_ctrl_pin_status`.
- `gpio_pin_status_mask` (INF 0x0400000A, Linux 0x4001000): **neither driver acts on it for this
  board.**
  - Windows stores it at ctx+0x2c8 and uses it only in FUN_00012378, which would drive every
    pin with a 0 mask bit low as an output. The loop exits at the first bit that equals
    XC_GPIO_I2C_SCL, XC_GPIO_I2C_SDA or tuner_reset_gpio_bit. The INF has all three commented
    out, their defaults are 0 / 0 / 0xff, and FUN_0001dc68 leaves the default when the value
    is missing, so the loop returns at bit 0 before any GPIO request.
  - Linux: the field appears only in the board table; no code reads it (grep over the tree).
- AGC select bit (INF 0x1c, Linux 0x0c):
  - Windows stores the byte at gpio+0x15 (FUN_00022f2c). FUN_00012dc8 then calls
    FUN_00022fe4(bit, 0) for BoardType 9 or 10: **GPIO 28 is made an output and driven low**.
    That is two SET_GPIO requests (bRequest 8, wValue 0x1000, wIndex 0, 4 data bytes 0).
    FUN_00012b08 repeats it only for tuner pin types.
  - Linux: `cx231xx_set_agc_analog_digital_mux_select` is called only for a list of models
    that does not include Elgato V2 (`cx231xx_dev_init`, `cx231xx_set_mode`), so 0x0c is never
    used and no GPIO is touched.
- XBarPin2 GpioMask / GpioSettings are stored in the pin node (+0x3c / +0x40) by FUN_00017be0;
  I found no reader **(inferred unused)**.

### 3.6 Areas with no differences found

Register read/write vendor requests; SET_GPIO; I2C wValue layout apart from the period;
BOARD_CFG_STAT scenario selection; `cx231xx_afe_init_channels`; DIF baseband; composite and
S-Video decoder input register values; I2S block; EP_MODE_SET start/stop; alt-setting choice
by width.

Not present at run time on this board: tuner object (`ctx+0x70` is only ever zeroed in the
decompilation), cx23416 object (`ctx+0x298`, needs Enable_SW_Encoder = 0), BDA filter.

---

## 4. Registry values

All are read with FUN_0001dc68(key, L"DriverData", name, &dword) -> FUN_0001ddcc (open) +
FUN_0001de78 (ZwQueryValueKey, accepts at most 4 bytes). On failure the caller's preset default
stays in place.

| Value (INF) | Lands in | Read by | Use |
|---|---|---|---|
| BoardType (0x0a) | `*(ushort*)(ctx+0x2a4)`, default 0x0c | FUN_00012dc8 (also FUN_000169c8) | Board-family switch. 3/6/7 = boards with a tuner-reset GPIO and SIF path; 3 or 6..11 = "all ADC channels on" in FUN_00022b38; 9/10 = AGC GPIO polarity 0; < 3 = polarity 1; 0x0b = extra stream type 7 |
| TunerType (0x28) | usbdev+0xd8, decoder+0x60, ctx+0x2a0 | FUN_00012dc8, FUN_0001bf48 | 0x1a disables vendor-request retries; 0x28 alters tuner-path bits in FUN_0001cd00 / FUN_0001dc0c. No effect on composite |
| I2CSpeed (1) | regio+0x50, ctx+0x24e | FUN_00012dc8 | I2C period bits. 3 is forced to 0 when bcdDevice == 0 |
| Enable_SW_Encoder (1) | ctx+0x250 (bool) | FUN_00012dc8 | 1 = no cx23416 object; selects filter descriptors in FUN_00013564 |
| Enable_BDA (0) | ctx+0x24f | FUN_00012dc8 | BDA filter creation |
| NoOfTuners (0) | ctx+0x32c | FUN_00012dc8, FUN_000138c4 | tuner / TV-audio filter factories |
| TunerI2CMaster (3) / DemodI2CMaster (2) | ctx+0x318 / ctx+0x319 | FUN_00012dc8 | which channel wrapper the tuner / demod objects get |
| BDA_Demod_Tuner_Type (0x0c) | ctx+0x1b0, default 2 | FUN_00012dc8 | BDA only |
| TunerI2CAddress / DemodI2CAddress | ctx+0x308 / ctx+0x30c | FUN_00012dc8 | tuner only |
| gpio_pin_status_mask (0x0400000A) | ctx+0x2c8, default 0xffffffff | FUN_00014048 | FUN_00012378 only; no effect as shipped (3.5) |
| AGC_Analog_Digitial_Select_Gpio_Bit (0x1c) | gpio+0x15 | FUN_00014048 | GPIO driven low at start (3.5) |
| demod_reset_gpio_bit, tuner_i2c_enable_gpio_bit, if_select_gpio_bit (absent) | gpio+0x10, +0x11, +0x12, default 0xff | FUN_00014048 | tuner / demod only |
| XC_GPIO_I2C_SCL / SDA (absent) | gpio+0x5c / +0x5d, default 0 | FUN_00014048, FUN_00012378 | bit-bang I2C pins |
| tuner_reset_gpio_bit, tuner_sif_fm_gpio_bit (absent) | tuner-gpio object +0x10 / +0x11 | FUN_00014048 | BoardType 3/6/7 only |
| VideoStandard (1) | decoder+0x08 | FUN_00014048 -> FUN_0001bf48; rewritten by FUN_000178f0 | KS AnalogVideo bitmask; 1 = NTSC-M |
| DisableVBI | ctx+0x3c | FUN_00014048 | stream setup |
| DisableAudioOutPin | ctx+0x40 (sets ctx+0x48 = 3 if non-zero) | FUN_00014048 | non-zero skips FUN_0001b000 on resume paths |
| EnableHANCAudioOut (0) | ctx+0x44 | FUN_00014048 | audio stream media type 1 vs 0x81 |
| ctrl_pin_status_mask (0xFFFFFFC4) | ctx+0x2cc | FUN_00014048 | FUN_00012440 |
| CpMethod (0xfe) | cp object +0x2c | FUN_00014048 | 0xfe = copy-protection poll thread FUN_000186dc is not started (FUN_00014e34) |
| TS1_Source (absent) | pcb config +4, default 0x20 | FUN_00014048 | TS1 register set in FUN_00026714 |
| EnableSliceCC | ctx+0x324 | FUN_00014048 | only with the cx23416 object |
| VideoInputPin (0) / AudioInputPin (2) | xbar+0x08 / xbar+0x0c | FUN_000179fc; rewritten by FUN_00018508 | initial crossbar route |
| ForceAudioWithVideoPin (1) | xbar+0x34 | FUN_000179fc | route related audio pin with video |
| XBarPinN\PinType | node+0x30; KS physical type at node+0x10 | FUN_00017be0 | 0 -> S-Video (3), 2 or 6 -> composite (2), 1/5/9 -> tuner (1), 4 -> audio line (0x1001), 3/10 -> audio tuner (0x1000), 7 -> YRYBY (5). PinType is also the "input type" passed to FUN_00012b08 / FUN_0001cd00 |
| XBarPinN\InputMux | node+0x34 | FUN_00017be0 | AFE mux bytes ch1 / ch2 / ch3 |
| XBarPinN\InputMux2, RelatedPinIndex, GpioMask, GpioSettings | node+0x38, +0x14, +0x3c, +0x40 | FUN_00017be0 | RelatedPinIndex used by FUN_00017f10; others no reader found |
| enable_2TS, BDA_Demod_GPIO_PIN, TunerCategory | locals | FUN_00015a68, FUN_00020474, FUN_000138c4 | BDA / tuner only |

INF values with no matching string in the binary (so not read by elvidcap.sys): I2SInputFormat,
EnableAFAudio, GpioMaskFM, GpioSettingFM, FMAudioGain, AnalogTunerXTALFreq, SawFilterType,
I2CMode, ShutdownTunerInCVBS, EnableAutoFormatDetection, DemodXTALFreqKHz.

Other derived fields: `usbdev+0x1c` = bcdDevice (0xff if descriptors not fetched, flag at
`usbdev+0x74`); `ctx+0x2c3` = pcb config byte 1 (power type, 0 = self powered); `ctx+0x1a0` =
pcb config byte 2 (USB speed); `ctx+0x2c4` = current power mode; `ctx+0x31c` = current video
input type; `ctx+0x320` = current audio input type; `ctx+0x210` = removed flag.

---

## 5. Firmware files

- **merlinD.rom: optional.** FUN_0001b000 calls FUN_0001b92c only when ZwCreateFile returns
  success and a non-null handle. Otherwise it goes straight on to the 0x914 / 0x918 / 0x808
  writes, the version read and the input switch; nothing checks that a download happened.
  The INF lists `merlind.bin` in `[SourceDisksFiles]` but no CopyFiles section installs it
  and nothing renames it to merlinD.rom.
  For line-in the driver never restarts the audio microcontroller: FUN_0001b620 (0x800 =
  0x13000000) is only called in the tuner branches of FUN_0001a978, so DL_CTL stays at
  0x03000000. Whether line audio is fully functional on hardware without the firmware is
  **(inferred, not verified)**.
- **cx416enc.rom: not reached on this board.** FUN_00025e20 (file read) is called only from
  FUN_000260b0, which is slot +0x40 of the stream-object vtable at 0x2c300. The only call
  sites go through `ctx+0x200` (FUN_00015944, FUN_00015c74), and the decompilation contains no
  store to `ctx+0x200` other than zeroing (constructor and teardown) **(inferred from absence
  of a store)**. Separately, the cx23416 object needs Enable_SW_Encoder = 0 and the INF sets 1.
  If it were reached, a missing file makes FUN_000260b0 return the ZwCreateFile error after
  clearing the EP enable bit.
