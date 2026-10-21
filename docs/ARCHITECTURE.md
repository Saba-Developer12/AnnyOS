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
the legacy i8042 keyboard for set 1 scan codes. Kernel commands are `DIR`,
`CD`, `CLS`, `ECHO`, and `COLOR`. `HELP`, `VER`, `MEM`, `DATE`, `TIME`, and
`REBOOT` now run as `.AP` applications through the versioned API. The date and
time API services read the CMOS RTC, while reboot uses the keyboard controller.
Keyboard initialization discards scan codes left over from firmware, and
editing redraws the prompt directly after the visible command history.

The first hand-written filesystem driver is FAT32 (`src/drivers/storage/fat32.cpp`).
It validates a FAT32 volume on a 512-byte block device and enumerates the root
directory using a caller-provided sector reader. It locates FAT32 volumes in
GPT EFI System Partitions or MBR FAT32 partitions. The kernel initializes the
primary-master legacy ATA PIO device and mounts the boot disk, so `DIR` lists
the current FAT32 directory and `CD` navigates short-name directories with
`..`, `\`, and multi-level paths. QEMU uses the `pc` machine model to expose
that IDE device. There is no dynamic driver loader yet.
The CLI loads version 1 `.AP` bytecode from its current FAT32 directory, then
falls back to `/APPS`. It uses `fat32_find_file` and `fat32_read_file`, then
validates the bounded 40-byte AP header, checksum, and instruction stream
before running it through `AnnyOsApiV1` (`include/annyos/api.hpp`). The startup
banner and help text plus `HELP`, `VER`, `MEM`, `DATE`, `TIME`, and `REBOOT`
are stored as AP applications under `/APPS`. AP is interpreted bytecode, not
native x86: AnnyOS does not yet have paging, user mode, or process isolation.
See `docs/AP_FORMAT.md`; the image also includes `/APPS/HELLO.AP` as a sample.
The requested seven-format set is FAT12, FAT16, FAT32, exFAT, ext2, ISO9660,
and UDF; FAT32 is the first implemented driver, with the remaining formats to
follow as storage milestones. The initial keyboard path is polling only;
interrupt-driven input, paging, persistent file operations, login, and userspace
remain later milestones. Freestanding memory primitives live in
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

The build scripts produce the UEFI application and combined image, but do not
create a disk image. The QEMU launch scripts require a bootable
`build/AnnyOS.img` supplied separately. The standard x64 fallback path is
`EFI/BOOT/BOOTX64.EFI`.
`bootx86.efi` and `EFI/BOOT/BOOTX86.EFI` are compatibility copies of that x64
PE image, not 32-bit EFI applications. `annyos.bin` is also a copy of the
current combined EFI loader/kernel image; it is not yet a separately loaded
kernel. `driver/ata.sys`, `driver/fat32.sys`, and `driver/ps2kbd.sys` are PE32+
modules with exported AnnyOS entry APIs. The current kernel still links the
active input and FAT32 code statically because module loading is a later step.

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
