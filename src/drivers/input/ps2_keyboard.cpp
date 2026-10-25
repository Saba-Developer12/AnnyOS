#include "annyos/ps2_keyboard.hpp"

namespace annyos::input {

namespace {
constexpr uint16_t kDataPort = 0x60;
constexpr uint16_t kStatusPort = 0x64;
constexpr uint8_t kOutputBufferFull = 1u << 0;
constexpr uint8_t kAuxiliaryData = 1u << 5;
constexpr uint8_t kTimeoutError = 1u << 6;
constexpr uint8_t kParityError = 1u << 7;

bool left_shift_down = false;
bool right_shift_down = false;
bool caps_lock = false;
bool extended_scancode = false;

uint8_t read_port(uint16_t port) {
    uint8_t value;
    __asm__ volatile("inb %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

char key_character(uint8_t code, bool shifted) {
    switch (code) {
        case 0x02: return shifted ? '!' : '1';
        case 0x03: return shifted ? '@' : '2';
        case 0x04: return shifted ? '#' : '3';
        case 0x05: return shifted ? '$' : '4';
        case 0x06: return shifted ? '%' : '5';
        case 0x07: return shifted ? '^' : '6';
        case 0x08: return shifted ? '&' : '7';
        case 0x09: return shifted ? '*' : '8';
        case 0x0a: return shifted ? '(' : '9';
        case 0x0b: return shifted ? ')' : '0';
        case 0x0c: return shifted ? '_' : '-';
        case 0x0d: return shifted ? '+' : '=';
        case 0x10: return 'q';
        case 0x11: return 'w';
        case 0x12: return 'e';
        case 0x13: return 'r';
        case 0x14: return 't';
        case 0x15: return 'y';
        case 0x16: return 'u';
        case 0x17: return 'i';
        case 0x18: return 'o';
        case 0x19: return 'p';
        case 0x1a: return shifted ? '{' : '[';
        case 0x1b: return shifted ? '}' : ']';
        case 0x1e: return 'a';
        case 0x1f: return 's';
        case 0x20: return 'd';
        case 0x21: return 'f';
        case 0x22: return 'g';
        case 0x23: return 'h';
        case 0x24: return 'j';
        case 0x25: return 'k';
        case 0x26: return 'l';
        case 0x27: return shifted ? ':' : ';';
        case 0x28: return shifted ? '"' : '\'';
        case 0x29: return shifted ? '~' : '`';
        case 0x2b: return shifted ? '|' : '\\';
        case 0x2c: return 'z';
        case 0x2d: return 'x';
        case 0x2e: return 'c';
        case 0x2f: return 'v';
        case 0x30: return 'b';
        case 0x31: return 'n';
        case 0x32: return 'm';
        case 0x33: return shifted ? '<' : ',';
        case 0x34: return shifted ? '>' : '.';
        case 0x35: return shifted ? '?' : '/';
        case 0x39: return ' ';
        default: return 0;
    }
}
}

void initialize_ps2_keyboard() {
    // Firmware/boot menus may leave scan codes queued. Discard only the
    // controller's current output buffer and reset decoder state before the
    // shell starts accepting input.
    for (unsigned drained = 0; drained < 32; ++drained) {
        const uint8_t status = read_port(kStatusPort);
        if ((status & kOutputBufferFull) == 0) break;
        (void)read_port(kDataPort);
    }
    left_shift_down = false;
    right_shift_down = false;
    caps_lock = false;
    extended_scancode = false;
}

bool poll_ps2_keyboard(KeyEvent* event) {
    if (event == nullptr) return false;
    const uint8_t status = read_port(kStatusPort);
    if ((status & kOutputBufferFull) == 0) return false;

    const uint8_t code = read_port(kDataPort);
    if ((status & (kAuxiliaryData | kTimeoutError | kParityError)) != 0) return false;
    if (code == 0xe0 || code == 0xe1) {
        extended_scancode = true;
        return false;
    }
    if (extended_scancode) {
        extended_scancode = false;
        const bool released = (code & 0x80) != 0;
        const uint8_t extended_key = code & 0x7f;
        if (!released) {
            switch (extended_key) {
                case 0x48: event->action = KeyAction::up; break;
                case 0x50: event->action = KeyAction::down; break;
                case 0x4b: event->action = KeyAction::left; break;
                case 0x4d: event->action = KeyAction::right; break;
                case 0x47: event->action = KeyAction::home; break;
                case 0x4f: event->action = KeyAction::end; break;
                case 0x53: event->action = KeyAction::delete_forward; break;
                default: return false;
            }
            event->character = 0;
            return true;
        }
        return false;
    }

    if (code == 0x01) {
        event->action = KeyAction::escape;
        event->character = 0;
        return true;
    }
    if (code == 0x3c) {
        event->action = KeyAction::save;
        event->character = 0;
        return true;
    }
    const bool released = (code & 0x80) != 0;
    const uint8_t key = code & 0x7f;
    if (key == 0x2a) {
        left_shift_down = !released;
        return false;
    }
    if (key == 0x36) {
        right_shift_down = !released;
        return false;
    }
    if (key == 0x3a) {
        if (!released) caps_lock = !caps_lock;
        return false;
    }
    if (released) return false;
    if (key == 0x1c) {
        event->action = KeyAction::enter;
        event->character = '\n';
        return true;
    }
    if (key == 0x0e) {
        event->action = KeyAction::backspace;
        event->character = '\b';
        return true;
    }

    const bool shift_down = left_shift_down || right_shift_down;
    char character = key_character(key, shift_down);
    if (character >= 'a' && character <= 'z') {
        if (shift_down != caps_lock) character = static_cast<char>(character - 'a' + 'A');
    }
    if (character == 0) return false;
    event->action = KeyAction::character;
    event->character = character;
    return true;
}

} // namespace annyos::input
