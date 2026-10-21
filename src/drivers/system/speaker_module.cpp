#include "annyos/pc_speaker.hpp"
#include "annyos/timer_pit.hpp"

extern "C" bool annyos_speaker_initialize() {
    return annyos::timer::initialize(1000);
}

extern "C" bool annyos_speaker_tone(uint32_t frequency_hz, uint32_t duration_ms) {
    return annyos::speaker::tone(frequency_hz, duration_ms);
}

extern "C" void annyos_speaker_stop() {
    annyos::speaker::stop();
}
