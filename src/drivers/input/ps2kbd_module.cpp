#include "annyos/ps2_keyboard.hpp"

extern "C" void annyos_ps2kbd_initialize() {
    annyos::input::initialize_ps2_keyboard();
}

extern "C" bool annyos_ps2kbd_poll(annyos::input::KeyEvent* event) {
    return annyos::input::poll_ps2_keyboard(event);
}
