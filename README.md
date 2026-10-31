# AnnyOS

Current version: **0.4.9**

AnnyOS is an experimental x86_64 bare-metal operating system with a UEFI boot
path. The boot application gathers GOP framebuffer and memory-map information,
exits UEFI Boot Services, and transfers control to freestanding C++ kernel code.
The project is an early development system; it is not yet a general-purpose OS.

## Current features

- x86_64 UEFI boot through `EFI/BOOT/BOOTX64.EFI` and a GOP framebuffer console.
- DOS-style command processor with `DIR`, `CD`, `CLS`, `COLOR`, `ECHO`, `SLEEP`, and `BEEP`, plus 32-entry command history with arrow-key recall.
- `COLOR` themes are stored as key/value settings in `ANNYOS.CFG` on the FAT32
  boot volume and restored at startup. Disk-image rebuilds preserve that file;
  `make clean` removes the image and its saved settings.
- PS/2 keyboard, FAT32/ATA PIO storage, 1000 Hz PIT timer, COM1 serial logging,
  and PC speaker tone drivers.
- Early xHCI controller discovery and HID boot-keyboard report decoding. USB
  keyboard enumeration/transport is not implemented yet; PS/2 remains the
  active input path. See [USB HID status](docs/USB_HID.md).
- FAT32 volumes on legacy ATA devices can be selected as `C:` through `F:`.
- Ethernet currently supports legacy PCI VirtIO-net and Intel 82540EM (`8086:100E`,
  including QEMU's e1000 model). The kernel selects VirtIO first, then falls back
  to e1000; `driver/VIRTIO.SYS` and `driver/E1000.SYS` are exported with the same
  polling frame interface. Wi-Fi chipsets and USB network adapters are not yet
  supported, and there is no generic driver that can work with every adapter.
- Disk listing and FAT32 space APIs are available to AADL applications through
  `DiskManagement.aadl`; the shell has no built-in `DISK` command. USB
  mass-storage support is not implemented yet.
- Text files can be created, read, edited, and deleted from the shell with
  `CREATE`, `READ`, `EDIT`, and `DELETE` in the current FAT32 directory.
- AP version 1 bytecode applications, including `HELP`, `VER`, `MEM`, `DATE`,
  `TIME`, and `REBOOT`.
- AADL preview compiler for classes, inheritance, compile-time objects/methods,
  recursive imports, and AP v1 output, with core libraries under `apps/lib/`.
  See [AADL documentation](docs/AADL.md).
- Segoe UI font assets rasterized into a freestanding kernel header.
- Build scripts for Windows PowerShell and Unix-like hosts, plus a QEMU launcher.

The upstream curl source is linked as the `curl/` submodule. In QEMU, the
shell's `CURL` `.AP` wrapper sends HTTP(S) requests over a COM2 bridge to the
upstream host curl CLI and displays or saves the response. Run `CURL -h` for
supported options. `make run` starts the bridge automatically; `make curl`
builds the upstream host binary. This development bridge is QEMU-only, not a
native bare-metal network client. See [curl integration notes](docs/CURL_PORTING.md).

FAT32 is the only filesystem currently mounted for file operations. NTFS and
APFS boot/container metadata can be detected, but neither is mounted yet.
FAT12, FAT16, exFAT, ext2, ISO9660, and UDF are planned. USB mass storage is
not supported yet. AP applications are interpreted bytecode; the
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

To exercise the Intel e1000 fallback in QEMU, launch with
`ANNYOS_QEMU_NIC=e1000,netdev=net0 make run` (or pass
`-NetworkDevice 'e1000,netdev=net0'` to `scripts/run_qemu.ps1`).

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
