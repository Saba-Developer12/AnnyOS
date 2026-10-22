# USB HID keyboard status

`usbhid.sys` is an early xHCI/HID foundation, not yet a usable USB keyboard
driver. It currently:

- scans PCI configuration space for xHCI host controllers and reports the PCI
  location and assigned MMIO BAR without dereferencing or changing it;
- decodes 8-byte HID boot-keyboard input reports into AnnyOS `KeyEvent`s,
  including basic US ASCII, shift, caps lock, Enter, Backspace, and arrow keys;
- is packaged as `build/esp/driver/usbhid.sys` with the exports
  `annyos_usbhid_probe` and `annyos_usbhid_decode_report`.

USB devices are not yet enumerated. There is no xHCI command/event ring,
transfer-ring setup, slot/context allocation, address/configuration request,
interrupt-IN endpoint polling, hub support, or connection-change handling.
Consequently, the kernel still reads interactive input from PS/2 and does not
receive reports from a USB keyboard. The report decoder is ready to be called
once that transport is implemented.

The decoder follows HID 1.11 boot-keyboard report behavior: reports are arrays,
so it compares each report against the previous one and emits at most one new
key-down event per call. HID boot protocol requires a `Set_Protocol` request
after the interface is configured; AnnyOS does not issue that request yet.
See the [USB-IF HID 1.11 specification](https://www.usb.org/sites/default/files/hid1_11.pdf).

The current UEFI page tables do not guarantee that the controller's MMIO BAR is
mapped into the address space visible after `ExitBootServices`. A future memory
manager must provide explicit physical-memory mapping and DMA-safe allocation
before the xHCI register and transfer layers can be enabled safely.
