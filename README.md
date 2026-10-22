# AnnyOS

Current version: **0.3.0**

AnnyOS is an experimental x86_64 bare-metal operating system with a UEFI boot
path. The boot application gathers GOP framebuffer and memory-map information,
exits UEFI Boot Services, and transfers control to freestanding C++ kernel code.
The project is an early development system; it is not yet a general-purpose OS.

## Current features

- x86_64 UEFI boot through `EFI/BOOT/BOOTX64.EFI` and a GOP framebuffer console.
- DOS-style command processor with `DIR`, `CD`, `CLS`, `COLOR`, `ECHO`, `SLEEP`, and `BEEP`, plus 32-entry command history with arrow-key recall.
- PS/2 keyboard, FAT32/ATA PIO storage, 1000 Hz PIT timer, COM1 serial logging,
  and PC speaker tone drivers.
- Early xHCI controller discovery and HID boot-keyboard report decoding. USB
  keyboard enumeration/transport is not implemented yet; PS/2 remains the
  active input path. See [USB HID status](docs/USB_HID.md).
- FAT32 volumes on legacy ATA devices can be selected as `C:` through `F:`.
- AP version 1 bytecode applications, including `HELP`, `VER`, `MEM`, `DATE`,
  `TIME`, and `REBOOT`.
- AADL preview compiler for classes, inheritance, compile-time objects/methods,
  recursive imports, and AP v1 output, with core libraries under `apps/lib/`.
  See [AADL documentation](docs/AADL.md).
- Segoe UI font assets rasterized into a freestanding kernel header.
- Build scripts for Windows PowerShell and Unix-like hosts, plus a QEMU launcher.

FAT32 is the only filesystem currently implemented. FAT12, FAT16, exFAT, ext2,
ISO9660, and UDF are planned. USB mass storage is not supported yet. AP applications are interpreted bytecode; the
kernel does not yet provide native process execution, user mode, paging, or
process isolation. See [the architecture notes](docs/ARCHITECTURE.md) and
[AP format documentation](docs/AP_FORMAT.md) for details.

## Build on Windows

Requirements: Clang, LLD, and QEMU with x64 EDK2/UEFI firmware.

```powershell
.\scripts\build.ps1
.\scripts\run_qemu.ps1
```

To select a non-default firmware image:

```powershell
.\scripts\run_qemu.ps1 -Firmware 'C:\path\to\edk2-x86_64-code.fd'
```

The build creates the EFI application under `build/esp/EFI/BOOT/BOOTX64.EFI`,
a combined image at `build/annyos.bin`, and a bootable FAT32 disk at
`build/AnnyOS.img`. The disk contains the boot files, drivers, fonts, and packed
`.AP` applications from `apps/`.

## Build on Linux or WSL

Requirements: Clang, LLD, QEMU, and x64 OVMF firmware.

```sh
make build
make run
```

Set `OVMF_CODE=/path/to/OVMF_CODE.fd` if the firmware is not in a standard
location. `make run` builds the FAT32 disk and boots it in QEMU.

## Repository layout

- `src/boot/` — UEFI entry point and GOP setup.
- `src/kernel/` — kernel, AP loader, and freestanding runtime.
- `src/drivers/` — keyboard, ATA PIO, and FAT32 code.
- `include/annyos/` — kernel interfaces and shared formats.
- `apps/` — AP assembly-like source programs.
- `font/segoe-ui/` — source font files; disk images store them under `/fonts/segoe-ui/`.
- `scripts/` — build, image, font-generation, and QEMU scripts.

## License and contributions

AnnyOS is authored by S-DEV. A repository license has not yet been added; do
not assume the source is covered by an open-source license until one is
provided.
