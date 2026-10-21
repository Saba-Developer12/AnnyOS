#include "annyos/timer_pit.hpp"

namespace annyos::timer {
namespace {
constexpr uint16_t kPitCommand = 0x43;
constexpr uint16_t kPitChannel0 = 0x40;
constexpr uint32_t kPitInputHz = 1193182;

uint32_t g_reload = 0;
uint32_t g_previous_count = 0;
uint64_t g_elapsed_clocks = 0;
bool g_initialized = false;

void out8(uint16_t port, uint8_t value) {
    __asm__ volatile("outb %0, %1" : : "a"(value), "Nd"(port));
}

uint8_t in8(uint16_t port) {
    uint8_t value;
    __asm__ volatile("inb %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

uint32_t read_counter() {
    out8(kPitCommand, 0x00); // Latch channel 0's current count.
    const uint16_t low = in8(kPitChannel0);
    const uint16_t high = in8(kPitChannel0);
    const uint32_t count = (high << 8) | low;
    return count == 0 ? g_reload : count;
}

uint64_t update_elapsed() {
    const uint32_t current = read_counter();
    if (current <= g_previous_count) {
        g_elapsed_clocks += g_previous_count - current;
    } else {
        g_elapsed_clocks += g_previous_count + (g_reload - current);
    }
    g_previous_count = current;
    return g_elapsed_clocks;
}
}

bool initialize(uint32_t frequency_hz) {
    if (frequency_hz == 0 || frequency_hz > kPitInputHz) return false;
    const uint32_t reload = kPitInputHz / frequency_hz;
    if (reload == 0 || reload > 0xffff) return false;

    g_reload = reload;
    g_elapsed_clocks = 0;
    out8(kPitCommand, 0x34); // Channel 0, lobyte/hibyte, rate generator.
    out8(kPitChannel0, static_cast<uint8_t>(reload));
    out8(kPitChannel0, static_cast<uint8_t>(reload >> 8));
    g_previous_count = read_counter();
    g_initialized = true;
    return true;
}

uint64_t ticks() {
    if (!g_initialized) return 0;
    return update_elapsed() / g_reload;
}

bool sleep_ms(uint32_t milliseconds) {
    if (!g_initialized) return false;
    const uint64_t target_clocks =
        (static_cast<uint64_t>(milliseconds) * kPitInputHz + 999) / 1000;
    const uint64_t start = update_elapsed();
    while (update_elapsed() - start < target_clocks) __asm__ volatile("pause");
    return true;
}

} // namespace annyos::timer
