#pragma once

#include <stdint.h>

namespace annyos::serial {

bool initialize(uint16_t base_port = 0x3f8);
bool write_byte(uint8_t byte);
bool write(const char* text);

} // namespace annyos::serial
