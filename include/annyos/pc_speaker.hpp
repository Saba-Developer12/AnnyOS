#pragma once

#include <stdint.h>

namespace annyos::speaker {

bool tone(uint32_t frequency_hz, uint32_t duration_ms);
void stop();

} // namespace annyos::speaker
