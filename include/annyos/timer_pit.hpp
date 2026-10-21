#pragma once

#include <stdint.h>

namespace annyos::timer {

bool initialize(uint32_t frequency_hz = 1000);
uint64_t ticks();
bool sleep_ms(uint32_t milliseconds);

} // namespace annyos::timer
