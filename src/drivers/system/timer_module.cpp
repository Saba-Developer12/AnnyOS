#include "annyos/timer_pit.hpp"

extern "C" bool annyos_timer_initialize(uint32_t frequency_hz) {
    return annyos::timer::initialize(frequency_hz);
}

extern "C" uint64_t annyos_timer_ticks() {
    return annyos::timer::ticks();
}

extern "C" bool annyos_timer_sleep_ms(uint32_t milliseconds) {
    return annyos::timer::sleep_ms(milliseconds);
}
