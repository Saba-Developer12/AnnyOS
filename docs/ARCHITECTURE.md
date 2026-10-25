# AnnyOS boot foundation

The first milestone is a single x64 UEFI PE application. The UEFI entry point
(`src/boot/efi_main.cpp`) locates GOP, captures the UEFI memory map, and exits
Boot Services. It then calls the separately compiled freestanding kernel entry
(`src/kernel/main.cpp`) using the x64 UEFI calling convention.

The kernel receives `annyos::BootInfo` from `include/annyos/boot_info.hpp`.
The memory map remains in its original UEFI descriptor format; consumers must
advance by `memory_descriptor_size`, not by a locally assumed structure size.
After `ExitBootServices`, the kernel does not call firmware services. It uses
the framebuffer, CPU instructions, PS/2 ports, and legacy ATA PIO ports.

The framebuffer renderer supports the GOP RGB, BGR, and bit-mask pixel formats.
The kernel draws a blue, DOS-style command screen with a `C:\>` prompt and polls
the legacy i8042 keyboard for set 1 scan codes. Kernel commands include `DIR`,
`CD`, `CREATE`, `EDIT`, `READ`, `DELETE`, `CLS`, `ECHO`, and `COLOR`. `HELP`, `VER`, `MEM`, `DATE`, `TIME`,
`REBOOT`, `SLEEP`, and `BEEP` run as `.AP` applications through the versioned
API. Date and time read the CMOS RTC; sleep uses the PIT timer, beep uses the
PC speaker, and reboot calls the UEFI Runtime `ResetSystem` service with chipset
and i8042 reset fallbacks.
Keyboard initialization discards scan codes left over from firmware, and
editing redraws the prompt directly after the visible command history.

The first hand-written filesystem driver is FAT32 (`src/drivers/storage/fat32.cpp`).
It validates a FAT32 volume on a 512-byte block device and enumerates the root
directory using a caller-provided sector reader. It locates FAT32 volumes in
GPT EFI System Partitions or MBR FAT32 partitions. The kernel initializes the
legacy ATA PIO positions and assigns C: through F: to mounted FAT32 disks.
`DIR` lists the current directory, `CD` navigates short-name directories with
`..`, `\`, and multi-level paths, and entering a drive name such as `D:` or
`E:\APPS` switches drives. QEMU uses the `pc` machine model to expose IDE
devices. USB mass storage is not supported yet because there is no USB host
controller or mass-storage driver. `CREATE`, `READ` (also `TYPE`), `EDIT`, and
`DELETE` (also `DEL`) operate on FAT 8.3 files in the current directory.
`CREATE NAME.TXT [TEXT]` creates an empty file or seeds it with one command
line; `EDIT NAME.TXT` opens a line-oriented editor with a 4 KiB text buffer,
`:REPLACE N TEXT`, `:DELETE N`, `:CLEAR`, `:SAVE`, and `:CANCEL` (line numbers
start at 1). `READ` displays text files up to 4 KiB, and
`DELETE` releases the file's FAT cluster chain. The FAT32 writer supports
multi-cluster replacement and empty files; the current shell editor is bounded
to 4 KiB and edits by line number rather than a movable cursor.
`src/drivers/storage/ntfs.cpp` and `apfs.cpp` provide validated format probes
for NTFS boot sectors and APFS container superblocks, and are packaged as
`/DRIVER/NTFS.SYS` and `/DRIVER/APFS.SYS`. They identify matching disks during
startup, but do not yet mount them or provide file operations; only FAT32 is
currently mounted by the shell. APFS probing verifies the container block
checksum, while NTFS probing validates its boot-sector geometry and MFT
locations.
The CLI loads version 1 `.AP` bytecode from its current FAT32 directory, then
falls back to `/APPS`. It uses `fat32_find_file` and `fat32_read_file`, then
validates the bounded 40-byte AP header, checksum, and instruction stream
before running it through `AnnyOsApiV1` (`include/annyos/api.hpp`). The startup
banner and help text plus `HELP`, `VER`, `MEM`, `DATE`, `TIME`, `REBOOT`,
`SLEEP`, and `BEEP`
are stored as AP applications under `/APPS`. AP is interpreted bytecode, not
native x86: AnnyOS does not yet have paging, user mode, or process isolation.
See `docs/AP_FORMAT.md`; the image also includes `/APPS/HELLO.AP` as a sample.
FAT32 is the only mounted filesystem. NTFS and APFS have metadata probes but
no file operations yet; FAT12, FAT16, exFAT, ext2, ISO9660, and UDF remain
future storage milestones. The initial keyboard path is polling only;
interrupt-driven input, paging, login, and userspace remain later milestones.
Freestanding memory primitives live in
`src/kernel/runtime.cpp` so Windows and Linux Clang builds can link without a
hosted C runtime.

The source TrueType files live in `font/segoe-ui/`. The console uses monochrome
rasterizations of the supplied Segoe UI Regular, Bold, Italic, and Bold Italic
faces. `tools/ttf_to_header.cpp` and
FreeType generate `include/annyos/font_data.hpp`; the freestanding kernel has no
font-library runtime dependency. HELP uses the bold face for its heading and
command labels, with cyan accents and aligned descriptions on the DOS blue
background. The four original `.ttf` files are also packaged at
`/fonts/segoe-ui/` in the FAT32 image.

The build scripts produce the UEFI application, combined image, and a
bootable FAT32 disk at `build/AnnyOS.img`. The disk packages the EFI loader,
drivers, fonts, and packed AP applications. The standard x64 fallback path is
`EFI/BOOT/BOOTX64.EFI`.
`bootx86.efi` and `EFI/BOOT/BOOTX86.EFI` are compatibility copies of that x64
PE image, not 32-bit EFI applications. `annyos.bin` is also a copy of the
current combined EFI loader/kernel image; it is not yet a separately loaded
kernel. `driver/ata.sys`, `driver/fat32.sys`, `driver/ps2kbd.sys`,
`driver/timer.sys`, `driver/serial.sys`, and `driver/speaker.sys` are PE32+
modules with exported AnnyOS entry APIs. The kernel statically links active
drivers; loading `.sys` modules at runtime is a later step. The PIT driver
provides polling-based ticks and busy-wait sleeps until interrupt handling is
implemented. COM1 logs are written to `build/serial.log` in QEMU.

## Built-in driver registry (current foundation)

`include/annyos/driver_registry.hpp` and `src/kernel/driver_registry.cpp`
provide a fixed-capacity registry for up to 128 built-in drivers and 128
versioned service interfaces. It uses no heap or hosted runtime. Each driver
declares service requirements, receives an initialization callback, and may
publish services only while it is being initialized. Missing dependencies
leave a driver blocked; callback failures are recorded and any services
published by that failed initialization are removed.

Kernel startup now registers COM1, PIT, PS/2 keyboard, ATA PIO, and FAT32
separately. FAT32 declares an ATA block-device service dependency, and the
command loop obtains keyboard input through the registry's versioned service
contract. This is a static startup registry, not a native `.sys` loader: the
current PE32+ `.sys` artifacts are packaged exports and are not mapped or
executed by the kernel. Runtime loading will require a defined module ABI,
relocation/import handling, memory ownership, unload rules, and fault
containment before external modules can be trusted.

AADL remains a bounded source compiler targeting AP v1 interpreted bytecode.
Its classes and inheritance are compile-time constructs; it does not yet
provide runtime objects, typed variables, parameters, branches, loops, or
native code generation. Those features need a new AP format version and a
validator/interpreter update before they can be safely exposed. See
`docs/ARCHITECTURE_ROADMAP.md` for staged expansion boundaries.

## Build and boot

On Windows, run `scripts/build.ps1` and then `scripts/run_qemu.ps1`. The build
requires Clang, LLD, WSL with Python 3 and the FAT image tools; the QEMU launcher
uses the installed x64 EDK2 firmware by default. Pass `-Firmware <path>` to
select another firmware image.

On a Unix-like host, `make build` or `scripts/build.sh` builds the same EFI
application and sample AP image. Python 3 and FAT image tools are needed to
create the disk. `OVMF_CODE` can select the firmware image for
`scripts/run_qemu.sh`.

The tagged-release workflow in `.github/workflows/release.yml` builds the disk
image and AnnyOS SDK archive when a `v*` tag is pushed. No release is published
by local builds.
