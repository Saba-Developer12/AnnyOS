#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
IMAGE="$ROOT/build/AnnyOS.img"
ESP="$ROOT/build/esp"
if [[ ! -f "$IMAGE" && ! -f "$ESP/EFI/BOOT/BOOTX64.EFI" ]]; then
  printf 'Boot files not found. Run make first to build %s.\n' "$ESP/EFI/BOOT/BOOTX64.EFI" >&2
  exit 1
fi
QEMU="${QEMU:-qemu-system-x86_64}"
FIRMWARE="${OVMF_CODE:-}"
if [[ -z "$FIRMWARE" ]]; then
  for candidate in \
    /usr/share/OVMF/OVMF_CODE_4M.fd \
    /usr/share/OVMF/OVMF_CODE.fd \
    /usr/share/edk2/ovmf/OVMF_CODE.fd \
    /usr/share/qemu/edk2-x86_64-code.fd; do
    if [[ -f "$candidate" ]]; then
      FIRMWARE="$candidate"
      break
    fi
  done
fi
if [[ ! -f "$FIRMWARE" ]]; then
  printf 'UEFI firmware not found. Install OVMF or set OVMF_CODE to an x64 firmware file.\n' >&2
  exit 1
fi
mkdir -p "$ROOT/build"
rm -f "$ROOT/build/qemu-debug.log"
rm -f "$ROOT/build/serial.log"
if [[ -f "$IMAGE" ]]; then
  DISK="if=ide,index=0,format=raw,file=$IMAGE"
else
  # QEMU's vvfat backend exposes the build directory as a writable FAT disk.
  # This boots the EFI application without a separate image creation utility.
  DISK="if=ide,index=0,format=raw,file=fat:rw:$ESP"
fi
"$QEMU" -machine pc -m 512M -smp 2 -drive "if=pflash,format=raw,readonly=on,file=$FIRMWARE" \
  -drive "$DISK" \
  -serial "file:$ROOT/build/serial.log" \
  -global isa-debugcon.iobase=0xe9 -debugcon "file:$ROOT/build/qemu-debug.log" \
  -no-reboot -no-shutdown "$@"
