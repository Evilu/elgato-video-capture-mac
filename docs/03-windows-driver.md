# 3. The Windows driver, `elvidcap.sys`

This chapter is the map of the decompiled driver. The detailed, address-by-address notes are in the two appendices; this is how to read them.

All addresses are for the x64 build (version 7.07.00.127, image base 0x10000 as loaded by Ghidra). `tools/fn.sh x64_elvidcap_sys FUN_xxxxxxxx` prints any function from your local export.

## 3.1 Shape of the binary

401 functions, all decompiled without failures. There are no symbols, but the code is C++ compiled from Conexant's reference AVStream driver, so it is organised as objects with vtables, and the vtables are what tie the pieces together.

| Area | Approximate share | What it is |
|---|---|---|
| AVStream / Kernel Streaming glue | about half | Filter and pin descriptors, format negotiation, property sets (crossbar, video decoder, picture controls, tuner), buffer handling |
| Device protocol | about a third | Vendor requests, register and I2C helpers, AFE, decoder, audio path, power modes |
| USB plumbing | about a tenth | Configuration and interface selection, URB building, completion routines, pipe recovery |
| Unused on this board | | Tuner and direct-IF programming, BDA (digital TV), CX23416 MPEG encoder firmware loader, bit-banged I2C over GPIO |

## 3.2 Object model

| Object | Created in | Purpose |
|---|---|---|
| Device context (0x330 bytes) | `FUN_00011f5c` | Root object; holds pointers to everything below and the values read from the registry |
| USB device | `FUN_00012dc8` | Descriptors, configuration handle, per-interface pipe information |
| Register / vendor-request object | `FUN_00012dc8` | The single routine that builds vendor requests is `FUN_00018b10` (URB function 0x17) |
| Board ("pcb") configuration | `FUN_0001ff84` | Chosen from bridge register 0x00; says which interface carries which stream |
| AFE | `FUN_00014048` | Analogue front end: `FUN_00022688` (super block), `FUN_000228b4` (channels), `FUN_00022b38` (power) |
| Video decoder | `FUN_00014048`, `FUN_0001bf48` | Standard select, input select (`FUN_0001cd00`), per-standard overrides (`FUN_0001d90c`), scaler (`FUN_0001d4f0`) |
| Audio decoder | `FUN_00014048` | Input select `FUN_0001a978`, sample-rate setup `FUN_0001ae0c`, late init `FUN_0001b000` |
| Crossbar | `FUN_000179fc` | Maps the INF's `XBarPin` entries to input routing (`FUN_00012b08`) |
| Stream (one per endpoint) | `FUN_00025858` | URB pool, worker thread `FUN_0002644c`, line assembler |
| Copy-protection monitor | `FUN_00014048` | Disabled by the shipped INF (`CpMethod` = 0xFE) |

## 3.3 Life of the device

1. **`DriverEntry`** (`FUN_00031ee4`) calls `KsInitializeDriver` with a device descriptor whose dispatch table gives Add = `FUN_00011f5c`, Start = `FUN_00011fa8`, SetPower = `FUN_0001267c`.
2. **Start** (`FUN_00012dc8`): read descriptors and select the configuration; read registry values; read the board configuration; set power mode 0x20; build and initialise the hardware objects (`FUN_00014048`: AFE, audio ADC block, decoder with the default standard and composite input, control pins); create the KS filter factories; build the crossbar, which routes the default input and switches power mode to 0x30; finish audio set-up (`FUN_0001b000`, which is also where the optional firmware file is tried).
3. **Pin to RUN** (`FUN_0001e4b4` → `FUN_00014e34`): program the scaler for the negotiated size, choose the alternate setting by width (`FUN_00015e34`), allocate and submit URBs and start the stream's worker thread (`FUN_00025c3c`), set the stream mode and endpoint-enable bits (`FUN_00026714`, `FUN_00026b6c`).
4. **Streaming** (`FUN_0002644c`): completed URBs are queued by the completion routine and parsed in the worker thread: `FUN_000277bc` walks each packet, `FUN_00027430` dispatches on the timing code, `FUN_00026f2c` accounts for lines and fields, and `FUN_0001e37c` stamps and completes the sample.
5. **Stop** (`FUN_0001515c`): clear the endpoint enable, stop the thread, abort the pipe, select alternate setting 0.

## 3.4 What the comparison with Linux showed

- **Transport, register access, the stream format and the parser are the same** in both drivers, function for function. Appendix B has the mapping table for the streaming side and appendix A for the control side.
- **Board configuration differs** in a dozen places: AFE reference count and a calibration step, ADC channel power, the S-Video chroma input, vertical timing values, and the audio stream mode write. They are tabulated in chapter 4, section 4.11, with which variant `libevc` uses.
- **Three things the Windows driver has that Linux lacks**: a pipe-recovery path after 50 consecutive failed URBs, parsing of ancillary packets (sliced closed captions and embedded audio), and the copy-protection monitor.
- **The video data is not processed at all** by the Windows driver: no colour conversion, scaling in software, or deinterlacing. The only operation on pixels is a `memcpy` into the woven frame. Whatever quality the device has is delivered to the application intact, which is why OBS on Windows gets a better result than a capture application that compresses.
- **No firmware is required.** `merlinD.rom` is loaded only if the file exists, and the installer does not ship it; the `cx416enc.rom` loader is unreachable with the shipped settings.

## 3.5 Reading Ghidra's output for this driver

A few patterns recur and are worth knowing before opening `decomp.c`:

- **Bit-field helpers appear as loops.** `set_field(mask, value)` compiles to `do { mask >>= 1; value *= 2; } while (!(mask & 1));`. With constant arguments the result is simply `value << (position of the mask's lowest set bit)`. `FUN_0001a978` contains more than twenty of these and reduces to five register writes.
- **I2C addresses are 7-bit** in this binary (0x44 decoder, 0x30 AFE, 0x4C audio ADC block); Linux writes the same addresses in 8-bit form (0x88, 0x60, 0x98).
- **`param_1[n]` on the device context is offset `8 * n`**, because Ghidra types the context as an array of 64-bit values.
- **Virtual calls** show as `(**(code **)(*obj + 0x18))(obj, …)`. Resolve them by reading the vtable from the binary at the address stored in the object's constructor.
- **Registry value names are the best anchors.** Each is referenced from exactly one place, the code that consumes it.

## 3.6 Appendices

- [Appendix A: control path](appendix-a-control-path.md): function map, the start-up call tree, every difference from Linux with addresses, registry values and where they land, firmware handling.
- [Appendix B: streaming path](appendix-b-data-path.md): alternate settings, URB geometry, the parser, advertised formats, audio, timestamps and threads, VBI and ancillary data, copy protection.
