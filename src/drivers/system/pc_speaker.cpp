#include "annyos/pc_speaker.hpp"
#include "annyos/timer_pit.hpp"

namespace annyos::speaker {
namespace {
constexpr uint16_t kPitCommand = 0x43;
constexpr uint16_t kPitChannel2 = 0x42;
constexpr uint16_t kSpeakerControl = 0x61;
constexpr uint32_t kPitInputHz = 1193182;

void out8(uint16_t port, uint8_t value) {
    __asm__ volatile("outb %0, %1" : : "a"(value), "Nd"(port));
}

uint8_t in8(uint16_t port) {
    uint8_t value;
    __asm__ volatile("inb %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}
}

void stop() {
    out8(kSpeakerControl, static_cast<uint8_t>(in8(kSpeakerControl) & ~0x03));
}

bool tone(uint32_t frequency_hz, uint32_t duration_ms) {
    if (frequency_hz < 20 || frequency_hz > 20000) return false;
    const uint32_t divisor = kPitInputHz / frequency_hz;
    if (divisor == 0 || divisor > 0xffff) return false;
    if (!annyos::timer::sleep_ms(0)) return false;

    out8(kPitCommand, 0xb6); // Channel 2, lobyte/hibyte, square wave.
    out8(kPitChannel2, static_cast<uint8_t>(divisor));
    out8(kPitChannel2, static_cast<uint8_t>(divisor >> 8));
    out8(kSpeakerControl, static_cast<uint8_t>(in8(kSpeakerControl) | 0x03));
    const bool elapsed = annyos::timer::sleep_ms(duration_ms);
    stop();
    return elapsed;
}

} // namespace annyos::speaker
