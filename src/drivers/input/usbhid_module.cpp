#include "annyos/usb_hid.hpp"

extern "C" bool annyos_usbhid_probe(annyos::input::usb::ControllerInfo* info) {
    return annyos::input::usb::probe_xhci(info);
}

extern "C" bool annyos_usbhid_decode_report(const uint8_t* report, uint32_t size,
                                              annyos::input::KeyEvent* event) {
    return annyos::input::usb::decode_boot_keyboard_report(report, size, event);
}
