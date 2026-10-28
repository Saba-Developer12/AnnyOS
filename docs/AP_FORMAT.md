# AnnyOS `.AP` application format, version 1

Version 1 applications are interpreted bytecode. They are not x86 machine
code or PE files. The kernel currently has no allocator, page isolation, ring 3
or process scheduler, so interpreting a small instruction set gives `.AP`
programs bounded operations through a kernel-trusted, versioned API. Programs
cannot directly address memory or issue hardware I/O instructions.

## File layout

All integer fields are little-endian. The 40-byte header is followed directly
by the bytecode.

| Offset | Size | Field | Version 1 value |
| ---: | ---: | --- | --- |
| 0 | 8 | Magic | `ANNYAP1\0` |
| 8 | 2 | Format version | `1` |
| 10 | 2 | Header size | `40` |
| 12 | 4 | Flags | `0` |
| 16 | 4 | Total file size | Exact length in bytes |
| 20 | 4 | Code offset | `40` |
| 24 | 4 | Code size | Exact bytecode length |
| 28 | 4 | Entry offset | `0` |
| 32 | 4 | Code checksum | FNV-1a 32-bit over bytecode |
| 36 | 4 | Minimum required AnnyOS API version | `1` |

The complete file is limited to 64 KiB. The loader reads integer fields from
the byte stream explicitly as little-endian values instead of casting the
untrusted image to a C++ struct. It checks exact file/header/code bounds,
checksum, opcode, operands, printable characters, instruction count and
terminal `EXIT` before emitting application output. FNV-1a detects accidental
corruption; it is not a signature or authenticity check.

## Bytecode

| Opcode | Encoding | Effect |
| --- | --- | --- |
| `0x01 PRINT` | `01`, one-byte length, that many printable ASCII bytes | Add one line to the command console |
| `0x02 API` | `02`, one-byte service ID | Call a versioned AnnyOS API service |
| `0x03 HELP_ROW` | `03`, command length, description length, then both ASCII strings | Draw one aligned, styled help row |
| `0x04 TITLE` | `04`, one-byte length, printable ASCII title | Draw a highlighted console title |
| `0x05 ARG` | `05`, zero-based argument index (`0`-`7`) | Print that argument; a missing argument prints an empty line |
| `0xFF EXIT` | `FF` | End the application; must be the final byte |

There are at most 4096 instructions, counting `EXIT`. Each `PRINT` line is
limited to 95 bytes.
API v1 service IDs are `1 VERSION`, `2 DATE`, `3 TIME`, `4 MEMORY`, `5 REBOOT`,
`6 SLEEP`, and `7 BEEP`. API v2 adds `8 CONFIG_LIST`, `9 CONFIG_GET_THEME`, and
`10`-`13 CONFIG_SET_THEME_BLUE/GREEN/AMBER/MATRIX`. API v3 adds service IDs
`14`-`67` for system/kernel, FAT32 file, bounded memory, audio, console graphics,
keyboard, serial, USB-controller probe, RTC, and disk operations. API v4 adds
service `68 CURL`, which accepts the bounded application arguments and forwards
them through the configured host curl bridge. `CURL.AP` is an AADL-compiled
wrapper; upstream curl itself runs on the host, not as guest-native code. The
complete names
and argument conventions are documented in `docs/AADL.md` and
`include/annyos/api.hpp`. `SLEEP` waits for 500 ms; `BEEP` plays the standard
short PC-speaker tone. ConfigSave settings are stored in `C:\ANNYOS.CFG`. The AP
file format remains v1; its minimum API version field is set to the highest
service version used by the application. V1 and v2 applications continue to
run on the newer host. Applications have no jumps, pointers, or native calls;
privileged effects are limited to the bounded API services. Filesystem calls
operate only on the current FAT32 directory. APFS/NTFS mounting, disk formatting,
mouse input, and cursor positioning are not implemented by the current kernel.

## Build and launch

`apps/*.aadl` contains the startup banner, command help, system applications,
and examples. `tools/aadlc.py` resolves imports recursively and compiles those
sources into `.AP` files, which the disk image builder places under `/APPS`.
Enter `CD APPS`, then `HELLO` to run the sample. Built-in command names also
search `/APPS` automatically, so `HELP`, `VER`, `DATE`, `TIME`, `MEM`,
`REBOOT`, `SLEEP`, and `BEEP` work from other directories. The command
processor accepts FAT 8.3 filenames and passes up to eight arguments to the
application. Arguments may be quoted with single or double quotes to include
spaces. In AADL, `arg(0);` prints the first argument; indexes are zero-based
and limited to `0` through `7`. FAT32 file commands include `CREATE`, `EDIT`,
`READ`/`TYPE`, and `DELETE`/`DEL`. `EDIT` is compiled from `apps/EDIT.aadl` to
`EDIT.AP` and opens the kernel's bounded editor through `UI_OPEN_EDITOR`; other
file operations use the corresponding bounded API services. Help rows are
defined in `apps/lib/FileSystem.aadl`.

The filesystem path is `fat32_find_file` followed by bounded
`fat32_read_file`. The CLI loads the file into a fixed 64 KiB kernel buffer,
then calls `application::execute_ap`. Later, native applications can be added
as a separate ABI after paging, user mode, process management and a syscall
boundary exist. The API is a kernel-trusted capability table, not a user-mode
security boundary; do not run untrusted applications that request privileged
services such as reboot.
