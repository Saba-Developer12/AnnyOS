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
CURL_BRIDGE_PORT="${ANNYOS_CURL_BRIDGE_PORT:-43210}"
QEMU_NIC="${ANNYOS_QEMU_NIC:-virtio-net-pci,netdev=net0,disable-modern=on}"
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
if [[ ! -x "$ROOT/build/bin/curl" ]]; then
  bash "$ROOT/scripts/build_curl_host.sh"
fi
mkdir -p "$ROOT/build"
rm -f "$ROOT/build/qemu-debug.log"
rm -f "$ROOT/build/serial.log"
python3 "$ROOT/scripts/curl_bridge.py" --port "$CURL_BRIDGE_PORT" \
  --curl "$ROOT/build/bin/curl" >"$ROOT/build/curl-bridge.log" 2>&1 &
CURL_BRIDGE_PID=$!
cleanup_curl_bridge() {
  kill "$CURL_BRIDGE_PID" 2>/dev/null || true
  wait "$CURL_BRIDGE_PID" 2>/dev/null || true
}
trap cleanup_curl_bridge EXIT INT TERM
sleep 0.25
if ! kill -0 "$CURL_BRIDGE_PID" 2>/dev/null; then
  cat "$ROOT/build/curl-bridge.log" >&2
  exit 1
fi
if [[ -f "$IMAGE" ]]; then
  DISK="if=ide,index=0,format=raw,file=$IMAGE"
else
  # QEMU's vvfat backend exposes the build directory as a writable FAT disk.
  # This boots the EFI application without a separate image creation utility.
  DISK="if=ide,index=0,format=raw,file=fat:rw:$ESP"
fi
"$QEMU" -machine pc -m 512M -smp 2 -drive "if=pflash,format=raw,readonly=on,file=$FIRMWARE" \
  -drive "$DISK" \
  -netdev user,id=net0 -device "$QEMU_NIC" \
  -chardev "socket,id=curlbridge,host=127.0.0.1,port=$CURL_BRIDGE_PORT,server=off" \
  -serial "file:$ROOT/build/serial.log" \
  -serial chardev:curlbridge \
  -global isa-debugcon.iobase=0xe9 -debugcon "file:$ROOT/build/qemu-debug.log" "$@"
