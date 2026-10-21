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
| 36 | 4 | Required AnnyOS API version | `1` |

The complete file is limited to 64 KiB. The loader checks every size and
offset, checksum, opcode, operand, printable character, instruction count and
terminal `EXIT` before it emits any application output. FNV-1a detects
accidental corruption; it is not a signature or authenticity check.

## Bytecode

| Opcode | Encoding | Effect |
| --- | --- | --- |
| `0x01 PRINT` | `01`, one-byte length, that many printable ASCII bytes | Add one line to the command console |
| `0x02 API` | `02`, one-byte service ID | Call a versioned AnnyOS API service |
| `0x03 HELP_ROW` | `03`, command length, description length, then both ASCII strings | Draw one aligned, styled help row |
| `0x04 TITLE` | `04`, one-byte length, printable ASCII title | Draw a highlighted console title |
| `0xFF EXIT` | `FF` | End the application; must be the final byte |

There are at most 4096 instructions. Each `PRINT` line is limited to 95 bytes.
API service IDs in version 1 are `1 VERSION`, `2 DATE`, `3 TIME`, `4 MEMORY`,
and `5 REBOOT`. The service table is defined in `include/annyos/api.hpp`.
Applications have no jumps, pointers, native calls, disk writes, or direct
hardware I/O; privileged actions are limited to these explicit API services.

## Build and launch

`apps/*.apasm` contains the startup banner, command help, and the `VER`, `DATE`,
`TIME`, `MEM`, and `REBOOT` commands that were previously hardcoded in
`main.cpp`. `tools/ap_pack.py` turns those sources into `.AP` files, which the
disk image builder places under `/APPS`. Enter `CD APPS`, then `HELLO` to run
the sample. Built-in command names also search `/APPS` automatically, so
`HELP`, `VER`, `DATE`, `TIME`, `MEM`, and `REBOOT` work from other directories.
The command processor accepts FAT 8.3 filenames and does not pass arguments.

The filesystem path is `fat32_find_file` followed by bounded
`fat32_read_file`. The CLI loads the file into a fixed 64 KiB kernel buffer,
then calls `application::execute_ap`. Later, native applications can be added
as a separate ABI after paging, user mode, process management and a syscall
boundary exist. The API is a kernel-trusted capability table, not a user-mode
security boundary; do not run untrusted applications that request privileged
services such as reboot.
