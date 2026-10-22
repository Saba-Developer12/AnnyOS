#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"
BUILD="build"
mkdir -p "$BUILD/obj" "$BUILD/esp/EFI/BOOT" "$BUILD/esp/fonts/segoe-ui"
COMMON=(--target=x86_64-pc-windows-msvc -std=c++20 -O2 -Wall -Wextra
  -ffreestanding -fno-builtin -fno-exceptions -fno-rtti -fno-stack-protector
  -fno-threadsafe-statics -fno-use-cxa-atexit -mno-red-zone -fshort-wchar
  -nostdlib -I"$ROOT/include" -I"$ROOT/src/boot")
clang++ "${COMMON[@]}" -c "$ROOT/src/boot/efi_main.cpp" -o "$BUILD/obj/efi_main.obj"
clang++ "${COMMON[@]}" -c "$ROOT/src/boot/gop_helper.cpp" -o "$BUILD/obj/gop_helper.obj"
clang++ "${COMMON[@]}" -c "$ROOT/src/drivers/input/ps2_keyboard.cpp" -o "$BUILD/obj/ps2_keyboard.obj"
clang++ "${COMMON[@]}" -c "$ROOT/src/drivers/input/usb_hid.cpp" -o "$BUILD/obj/usb_hid.obj"
clang++ "${COMMON[@]}" -c "$ROOT/src/drivers/storage/fat32.cpp" -o "$BUILD/obj/fat32.obj"
clang++ "${COMMON[@]}" -c "$ROOT/src/drivers/storage/ata_pio.cpp" -o "$BUILD/obj/ata_pio.obj"
clang++ "${COMMON[@]}" -c "$ROOT/src/drivers/system/timer_pit.cpp" -o "$BUILD/obj/timer_pit.obj"
clang++ "${COMMON[@]}" -c "$ROOT/src/drivers/system/serial_com1.cpp" -o "$BUILD/obj/serial_com1.obj"
clang++ "${COMMON[@]}" -c "$ROOT/src/drivers/system/pc_speaker.cpp" -o "$BUILD/obj/pc_speaker.obj"
clang++ "${COMMON[@]}" -c "$ROOT/src/kernel/ap_loader.cpp" -o "$BUILD/obj/ap_loader.obj"
clang++ "${COMMON[@]}" -c "$ROOT/src/drivers/storage/fat32_module.cpp" -o "$BUILD/obj/fat32_module.obj"
clang++ "${COMMON[@]}" -c "$ROOT/src/drivers/input/ps2kbd_module.cpp" -o "$BUILD/obj/ps2kbd_module.obj"
clang++ "${COMMON[@]}" -c "$ROOT/src/drivers/input/usbhid_module.cpp" -o "$BUILD/obj/usbhid_module.obj"
clang++ "${COMMON[@]}" -c "$ROOT/src/drivers/system/timer_module.cpp" -o "$BUILD/obj/timer_module.obj"
clang++ "${COMMON[@]}" -c "$ROOT/src/drivers/system/serial_module.cpp" -o "$BUILD/obj/serial_module.obj"
clang++ "${COMMON[@]}" -c "$ROOT/src/drivers/system/speaker_module.cpp" -o "$BUILD/obj/speaker_module.obj"
clang++ "${COMMON[@]}" -c "$ROOT/src/kernel/runtime.cpp" -o "$BUILD/obj/runtime.obj"
clang++ "${COMMON[@]}" -c "$ROOT/src/kernel/main.cpp" -o "$BUILD/obj/kernel_main.obj"
# Relative paths work with native POSIX LLD. Excluding MSYS argument conversion
# also keeps Git Bash from rewriting LLD's slash-prefixed linker options.
MSYS2_ARG_CONV_EXCL='*' MSYS_ARG_CONV_EXCL='*' lld-link \
  /subsystem:efi_application /entry:efi_main /nodefaultlib /machine:x64 \
  /dynamicbase /nxcompat /opt:ref /timestamp:0 "/out:$BUILD/esp/EFI/BOOT/BOOTX64.EFI" \
  "$BUILD/obj/efi_main.obj" "$BUILD/obj/gop_helper.obj" \
  "$BUILD/obj/ps2_keyboard.obj" "$BUILD/obj/fat32.obj" \
  "$BUILD/obj/usb_hid.obj" \
  "$BUILD/obj/ata_pio.obj" "$BUILD/obj/timer_pit.obj" \
  "$BUILD/obj/serial_com1.obj" "$BUILD/obj/pc_speaker.obj" \
  "$BUILD/obj/ap_loader.obj" \
  "$BUILD/obj/runtime.obj" "$BUILD/obj/kernel_main.obj"

MSYS2_ARG_CONV_EXCL='*' MSYS_ARG_CONV_EXCL='*' lld-link /dll /noentry /nodefaultlib /machine:x64 \
  /dynamicbase /timestamp:0 /export:annyos_fat32_mount /export:annyos_fat32_list_root \
  /export:annyos_fat32_list_directory /export:annyos_fat32_find_file \
  /export:annyos_fat32_read_file \
  "/out:$BUILD/fat32.sys" "$BUILD/obj/fat32.obj" "$BUILD/obj/fat32_module.obj"
MSYS2_ARG_CONV_EXCL='*' MSYS_ARG_CONV_EXCL='*' lld-link /dll /noentry /nodefaultlib /machine:x64 \
  /dynamicbase /timestamp:0 /export:annyos_ata_pio_initialize /export:annyos_ata_pio_read_sector \
  "/out:$BUILD/ata.sys" "$BUILD/obj/ata_pio.obj"
MSYS2_ARG_CONV_EXCL='*' MSYS_ARG_CONV_EXCL='*' lld-link /dll /noentry /nodefaultlib /machine:x64 \
  /dynamicbase /timestamp:0 /export:annyos_ps2kbd_initialize /export:annyos_ps2kbd_poll \
  "/out:$BUILD/ps2kbd.sys" "$BUILD/obj/ps2_keyboard.obj" "$BUILD/obj/ps2kbd_module.obj"
MSYS2_ARG_CONV_EXCL='*' MSYS_ARG_CONV_EXCL='*' lld-link /dll /noentry /nodefaultlib /machine:x64 \
  /dynamicbase /timestamp:0 /export:annyos_usbhid_probe /export:annyos_usbhid_decode_report \
  "/out:$BUILD/usbhid.sys" "$BUILD/obj/usb_hid.obj" "$BUILD/obj/usbhid_module.obj"
MSYS2_ARG_CONV_EXCL='*' MSYS_ARG_CONV_EXCL='*' lld-link /dll /noentry /nodefaultlib /machine:x64 \
  /dynamicbase /timestamp:0 /export:annyos_timer_initialize /export:annyos_timer_ticks \
  /export:annyos_timer_sleep_ms \
  "/out:$BUILD/timer.sys" "$BUILD/obj/timer_pit.obj" "$BUILD/obj/timer_module.obj"
MSYS2_ARG_CONV_EXCL='*' MSYS_ARG_CONV_EXCL='*' lld-link /dll /noentry /nodefaultlib /machine:x64 \
  /dynamicbase /timestamp:0 /export:annyos_serial_initialize /export:annyos_serial_write_byte \
  /export:annyos_serial_write \
  "/out:$BUILD/serial.sys" "$BUILD/obj/serial_com1.obj" "$BUILD/obj/serial_module.obj"
MSYS2_ARG_CONV_EXCL='*' MSYS_ARG_CONV_EXCL='*' lld-link /dll /noentry /nodefaultlib /machine:x64 \
  /dynamicbase /timestamp:0 /export:annyos_speaker_initialize \
  /export:annyos_speaker_tone /export:annyos_speaker_stop \
  "/out:$BUILD/speaker.sys" "$BUILD/obj/pc_speaker.obj" \
  "$BUILD/obj/timer_pit.obj" "$BUILD/obj/speaker_module.obj"

cp "$BUILD/esp/EFI/BOOT/BOOTX64.EFI" "$BUILD/annyos.bin"
cp "$BUILD/esp/EFI/BOOT/BOOTX64.EFI" "$BUILD/esp/EFI/BOOT/BOOTX86.EFI"
mkdir -p "$BUILD/esp/driver"
cp "$BUILD/annyos.bin" "$BUILD/esp/ANNYOS.BIN"
cp "$BUILD/esp/EFI/BOOT/BOOTX64.EFI" "$BUILD/esp/bootx86.efi"
cp "$ROOT/font/segoe-ui/Segoe UI.ttf" "$ROOT/font/segoe-ui/Segoe UI Bold.ttf" \
  "$ROOT/font/segoe-ui/Segoe UI Italic.ttf" "$ROOT/font/segoe-ui/Segoe UI Bold Italic.ttf" \
  "$BUILD/esp/fonts/segoe-ui/"
cp "$ROOT/font/segoe-ui/README.TXT" "$BUILD/esp/fonts/segoe-ui/"
cp "$BUILD/ata.sys" "$BUILD/fat32.sys" "$BUILD/ps2kbd.sys" "$BUILD/usbhid.sys" \
  "$BUILD/timer.sys" "$BUILD/serial.sys" "$BUILD/speaker.sys" "$BUILD/esp/driver/"
mkdir -p "$BUILD/esp/apps"
for source in "$ROOT"/apps/*.aadl; do
  [ -f "$source" ] || continue
  output="$BUILD/esp/apps/$(basename "${source%.*}").AP"
  python3 "$ROOT/tools/aadlc.py" "$source" "$output"
done
python3 "$ROOT/tools/build_disk_image.py" "$BUILD/esp" "$BUILD/AnnyOS.img"
printf 'Built %s\n' "$BUILD/esp/EFI/BOOT/BOOTX64.EFI"
