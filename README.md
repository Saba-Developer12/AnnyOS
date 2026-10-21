# AnnyOS

AnnyOS is an experimental x86_64 bare-metal operating system with a UEFI boot
path. The boot application gathers GOP framebuffer and memory-map information,
exits UEFI Boot Services, and transfers control to freestanding C++ kernel code.
The project is an early development system; it is not yet a general-purpose OS.

## Current features

- x86_64 UEFI boot through `EFI/BOOT/BOOTX64.EFI` and a GOP framebuffer console.
- DOS-style command processor with `DIR`, `CD`, `CLS`, `COLOR`, and `ECHO`.
- PS/2 keyboard input and a FAT32 driver backed by legacy ATA PIO.
- AP version 1 bytecode applications, including `HELP`, `VER`, `MEM`, `DATE`,
  `TIME`, and `REBOOT`.
- Segoe UI font assets rasterized into a freestanding kernel header.
- Build scripts for Windows PowerShell and Unix-like hosts, plus a QEMU launcher.

FAT32 is the only filesystem currently implemented. FAT12, FAT16, exFAT, ext2,
ISO9660, and UDF are planned. AP applications are interpreted bytecode; the
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

The build creates the EFI application under `build/esp/EFI/BOOT/BOOTX64.EFI`
and a combined image at `build/annyos.bin`. Image creation scripts are not
included, so `run_qemu.ps1` requires a bootable `build/AnnyOS.img` supplied
separately.

## Build on Linux or WSL

Requirements: Clang, LLD, QEMU, and x64 OVMF firmware.

```sh
make build
make run
```

Set `OVMF_CODE=/path/to/OVMF_CODE.fd` if the firmware is not in a standard
location. `make run` requires a bootable `build/AnnyOS.img` supplied separately.

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
