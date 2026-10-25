#include "annyos/serial.hpp"

extern "C" bool annyos_serial_initialize(uint16_t base_port) {
    return annyos::serial::initialize(base_port);
}

extern "C" bool annyos_serial_write_byte(uint8_t byte) {
    return annyos::serial::write_byte(byte);
}

extern "C" bool annyos_serial_write(const char* text) {
    return annyos::serial::write(text);
}

extern "C" bool annyos_serial_read_byte(uint8_t* byte) {
    return annyos::serial::read_byte(byte);
}
