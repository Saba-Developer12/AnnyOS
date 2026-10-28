#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"
BUILD="build"
mkdir -p "$BUILD/obj" "$BUILD/esp/EFI/BOOT" "$BUILD/esp/fonts/segoe-ui"
COMMON=(--target=x86_64-pc-windows-msvc -std=c++20 -O2 -Wall -Wextra
  -ffreestanding -fno-builtin -fno-exceptions -fno-rtti -fno-stack-protector
  -fno-threadsafe-statics -fno-use-cxa-atexit -mno-red-zone -fshort-wchar
  -nostdlib -I"$ROOT/include" -I"$ROOT/src/boot"
  -I"$ROOT/ports/lwip/include" -I"$ROOT/third_party/lwip/src/include")
clang++ "${COMMON[@]}" -c "$ROOT/src/boot/efi_main.cpp" -o "$BUILD/obj/efi_main.obj"
clang++ "${COMMON[@]}" -c "$ROOT/src/boot/gop_helper.cpp" -o "$BUILD/obj/gop_helper.obj"
clang++ "${COMMON[@]}" -c "$ROOT/src/drivers/input/ps2_keyboard.cpp" -o "$BUILD/obj/ps2_keyboard.obj"
clang++ "${COMMON[@]}" -c "$ROOT/src/drivers/input/usb_hid.cpp" -o "$BUILD/obj/usb_hid.obj"
clang++ "${COMMON[@]}" -c "$ROOT/src/drivers/storage/fat32.cpp" -o "$BUILD/obj/fat32.obj"
clang++ "${COMMON[@]}" -c "$ROOT/src/drivers/storage/ntfs.cpp" -o "$BUILD/obj/ntfs.obj"
clang++ "${COMMON[@]}" -c "$ROOT/src/drivers/storage/apfs.cpp" -o "$BUILD/obj/apfs.obj"
clang++ "${COMMON[@]}" -c "$ROOT/src/drivers/storage/ata_pio.cpp" -o "$BUILD/obj/ata_pio.obj"
clang++ "${COMMON[@]}" -c "$ROOT/src/drivers/network/virtio_net.cpp" -o "$BUILD/obj/virtio_net.obj"
clang++ "${COMMON[@]}" -c "$ROOT/src/kernel/network_stack.cpp" -o "$BUILD/obj/network_stack.obj"
clang++ "${COMMON[@]}" -c "$ROOT/src/kernel/curl_host_bridge.cpp" -o "$BUILD/obj/curl_host_bridge.obj"
clang++ "${COMMON[@]}" -c "$ROOT/src/drivers/system/timer_pit.cpp" -o "$BUILD/obj/timer_pit.obj"
clang++ "${COMMON[@]}" -c "$ROOT/src/drivers/system/serial_com1.cpp" -o "$BUILD/obj/serial_com1.obj"
clang++ "${COMMON[@]}" -c "$ROOT/src/drivers/system/pc_speaker.cpp" -o "$BUILD/obj/pc_speaker.obj"
clang++ "${COMMON[@]}" -c "$ROOT/src/kernel/ap_loader.cpp" -o "$BUILD/obj/ap_loader.obj"
clang++ "${COMMON[@]}" -c "$ROOT/src/kernel/config_save.cpp" -o "$BUILD/obj/config_save.obj"
clang++ "${COMMON[@]}" -c "$ROOT/src/kernel/driver_registry.cpp" -o "$BUILD/obj/driver_registry.obj"
clang++ "${COMMON[@]}" -c "$ROOT/src/kernel/shell.cpp" -o "$BUILD/obj/shell.obj"
clang++ "${COMMON[@]}" -c "$ROOT/src/kernel/shell_files.cpp" -o "$BUILD/obj/shell_files.obj"
clang++ "${COMMON[@]}" -c "$ROOT/src/kernel/shell_ui.cpp" -o "$BUILD/obj/shell_ui.obj"
clang++ "${COMMON[@]}" -c "$ROOT/src/drivers/storage/fat32_module.cpp" -o "$BUILD/obj/fat32_module.obj"
clang++ "${COMMON[@]}" -c "$ROOT/src/drivers/storage/ntfs_module.cpp" -o "$BUILD/obj/ntfs_module.obj"
clang++ "${COMMON[@]}" -c "$ROOT/src/drivers/storage/apfs_module.cpp" -o "$BUILD/obj/apfs_module.obj"
clang++ "${COMMON[@]}" -c "$ROOT/src/drivers/input/ps2kbd_module.cpp" -o "$BUILD/obj/ps2kbd_module.obj"
clang++ "${COMMON[@]}" -c "$ROOT/src/drivers/input/usbhid_module.cpp" -o "$BUILD/obj/usbhid_module.obj"
clang++ "${COMMON[@]}" -c "$ROOT/src/drivers/system/timer_module.cpp" -o "$BUILD/obj/timer_module.obj"
clang++ "${COMMON[@]}" -c "$ROOT/src/drivers/system/serial_module.cpp" -o "$BUILD/obj/serial_module.obj"
clang++ "${COMMON[@]}" -c "$ROOT/src/drivers/system/speaker_module.cpp" -o "$BUILD/obj/speaker_module.obj"
clang++ "${COMMON[@]}" -c "$ROOT/src/kernel/runtime.cpp" -o "$BUILD/obj/runtime.obj"
clang++ "${COMMON[@]}" -c "$ROOT/src/kernel/main.cpp" -o "$BUILD/obj/kernel_main.obj"
LWIP_SOURCES=(
  src/core/init.c src/core/def.c src/core/dns.c src/core/inet_chksum.c
  src/core/ip.c src/core/mem.c src/core/memp.c src/core/netif.c src/core/pbuf.c
  src/core/raw.c src/core/stats.c src/core/sys.c src/core/altcp.c
  src/core/altcp_alloc.c src/core/altcp_tcp.c src/core/tcp.c src/core/tcp_in.c
  src/core/tcp_out.c src/core/timeouts.c src/core/udp.c src/core/ipv4/acd.c
  src/core/ipv4/dhcp.c src/core/ipv4/etharp.c src/core/ipv4/icmp.c
  src/core/ipv4/ip4.c src/core/ipv4/ip4_addr.c src/core/ipv4/ip4_frag.c
  src/netif/ethernet.c
)
LWIP_COMMON=(--target=x86_64-pc-windows-msvc -std=c11 -O2 -Wall -Wextra
  -ffreestanding -fno-builtin -fno-stack-protector -mno-red-zone -nostdlib
  -I"$ROOT/ports/lwip/include" -I"$ROOT/third_party/lwip/src/include")
LWIP_OBJECTS=()
for source in "${LWIP_SOURCES[@]}"; do
  object_name="lwip_$(basename "${source%.c}").obj"
  # Source basenames are unique in this explicitly selected lwIP subset.
  clang "${LWIP_COMMON[@]}" -c "$ROOT/third_party/lwip/$source" -o "$BUILD/obj/$object_name"
  LWIP_OBJECTS+=("$BUILD/obj/$object_name")
done
# Relative paths work with native POSIX LLD. Excluding MSYS argument conversion
# also keeps Git Bash from rewriting LLD's slash-prefixed linker options.
MSYS2_ARG_CONV_EXCL='*' MSYS_ARG_CONV_EXCL='*' lld-link \
  /subsystem:efi_application /entry:efi_main /nodefaultlib /machine:x64 \
  /dynamicbase /nxcompat /opt:ref /timestamp:0 "/out:$BUILD/esp/EFI/BOOT/BOOTX64.EFI" \
  "$BUILD/obj/efi_main.obj" "$BUILD/obj/gop_helper.obj" \
  "$BUILD/obj/ps2_keyboard.obj" "$BUILD/obj/fat32.obj" \
  "$BUILD/obj/ntfs.obj" "$BUILD/obj/apfs.obj" \
  "$BUILD/obj/usb_hid.obj" \
  "$BUILD/obj/ata_pio.obj" "$BUILD/obj/virtio_net.obj" "$BUILD/obj/timer_pit.obj" \
  "$BUILD/obj/serial_com1.obj" "$BUILD/obj/pc_speaker.obj" \
  "$BUILD/obj/ap_loader.obj" \
  "$BUILD/obj/config_save.obj" \
  "$BUILD/obj/driver_registry.obj" \
  "$BUILD/obj/shell.obj" "$BUILD/obj/shell_files.obj" "$BUILD/obj/shell_ui.obj" \
  "$BUILD/obj/runtime.obj" "$BUILD/obj/network_stack.obj" \
  "$BUILD/obj/curl_host_bridge.obj" "$BUILD/obj/kernel_main.obj" \
  "${LWIP_OBJECTS[@]}"

MSYS2_ARG_CONV_EXCL='*' MSYS_ARG_CONV_EXCL='*' lld-link /dll /noentry /nodefaultlib /machine:x64 \
  /dynamicbase /timestamp:0 /export:annyos_fat32_mount /export:annyos_fat32_list_root \
  /export:annyos_fat32_list_directory /export:annyos_fat32_find_file \
  /export:annyos_fat32_read_file /export:annyos_fat32_write_file \
  /export:annyos_fat32_delete_file \
  "/out:$BUILD/fat32.sys" "$BUILD/obj/fat32.obj" "$BUILD/obj/fat32_module.obj"
MSYS2_ARG_CONV_EXCL='*' MSYS_ARG_CONV_EXCL='*' lld-link /dll /noentry /nodefaultlib /machine:x64 \
  /dynamicbase /timestamp:0 /export:annyos_ntfs_probe \
  "/out:$BUILD/ntfs.sys" "$BUILD/obj/ntfs.obj" "$BUILD/obj/ntfs_module.obj"
MSYS2_ARG_CONV_EXCL='*' MSYS_ARG_CONV_EXCL='*' lld-link /dll /noentry /nodefaultlib /machine:x64 \
  /dynamicbase /timestamp:0 /export:annyos_apfs_probe \
  "/out:$BUILD/apfs.sys" "$BUILD/obj/apfs.obj" "$BUILD/obj/apfs_module.obj"
MSYS2_ARG_CONV_EXCL='*' MSYS_ARG_CONV_EXCL='*' lld-link /dll /noentry /nodefaultlib /machine:x64 \
  /dynamicbase /timestamp:0 /export:annyos_ata_pio_initialize /export:annyos_ata_pio_read_sector \
  /export:annyos_ata_pio_write_sector \
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
cp "$BUILD/ata.sys" "$BUILD/fat32.sys" "$BUILD/ntfs.sys" "$BUILD/apfs.sys" \
  "$BUILD/ps2kbd.sys" "$BUILD/usbhid.sys" \
  "$BUILD/timer.sys" "$BUILD/serial.sys" "$BUILD/speaker.sys" "$BUILD/esp/driver/"
mkdir -p "$BUILD/esp/apps"
for source in "$ROOT"/apps/*.aadl; do
  [ -f "$source" ] || continue
  output="$BUILD/esp/apps/$(basename "${source%.*}").AP"
  python3 "$ROOT/tools/aadlc.py" "$source" "$output"
done
python3 "$ROOT/tools/build_disk_image.py" "$BUILD/esp" "$BUILD/AnnyOS.img"
printf 'Built %s\n' "$BUILD/esp/EFI/BOOT/BOOTX64.EFI"
