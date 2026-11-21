# Documentation

| Chapter | Contents |
|---|---|
| [1. Drivers and porting](01-drivers-and-porting.md) | How capture drivers are built on Windows and macOS, why this is a reimplementation and not a recompile, the pipeline, the method |
| [2. The installer and the hardware](02-installer-and-hardware.md) | What the installer contains, the two hardware generations, the INF's settings, USB descriptors, firmware, the chip's internal blocks |
| [3. The Windows driver](03-windows-driver.md) | Map of the decompiled `elvidcap.sys`: objects, life cycle, comparison with Linux, how to read the Ghidra output |
| [4. Device protocol](04-device-protocol.md) | The hardware's behaviour as a specification: requests, registers, bring-up, stream formats, where the two reference drivers disagree |
| [5. The macOS driver](05-macos-driver.md) | Architecture, building, using it in OBS, what is verified and what is not, next steps, licence |
| [Appendix A](appendix-a-control-path.md) | Working notes: control path of `elvidcap.sys`, function by function |
| [Appendix B](appendix-b-data-path.md) | Working notes: streaming path of `elvidcap.sys`, function by function |

Nothing here reproduces Elgato's code. The appendices cite function addresses in the vendor binary and describe behaviour; to follow them you need your own copy of the installer and `./build.sh analyse`.
