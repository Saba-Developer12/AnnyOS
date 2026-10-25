#pragma once

#include <stdint.h>

namespace annyos::input {

enum class KeyAction : uint8_t {
    character,
    enter,
    backspace,
    delete_forward,
    up,
    down,
    left,
    right,
    home,
    end,
    escape,
    save,
};

struct KeyEvent {
    KeyAction action;
    char character;
};

// Poll the legacy i8042 keyboard data port. This driver expects the controller
// to translate keyboard scan codes to set 1, as it does on the QEMU PC platform.
void initialize_ps2_keyboard();
bool poll_ps2_keyboard(KeyEvent* event);

} // namespace annyos::input
