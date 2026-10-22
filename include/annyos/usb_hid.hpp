#pragma once

#include <stdint.h>

#include "annyos/ps2_keyboard.hpp"

namespace annyos::input::usb {

struct ControllerInfo {
    uint64_t mmio_base;
    uint8_t bus;
    uint8_t device;
    uint8_t function;
    uint16_t pci_xhci_matches;
};

// Finds an xHCI PCI function and records its assigned MMIO BAR. It does not
// dereference the BAR or take ownership of/reset firmware's controller.
bool probe_xhci(ControllerInfo* info);

// Decode one USB HID boot-keyboard input report. Returns a key-down event when
// a new key appears in the report; key releases update state without emitting.
bool decode_boot_keyboard_report(const uint8_t* report, uint32_t report_size,
                                 KeyEvent* event);
void reset_boot_keyboard_decoder();

} // namespace annyos::input::usb
