#include "annyos/serial.hpp"

namespace annyos::serial {
namespace {
constexpr uint8_t kData = 0;
constexpr uint8_t kInterruptEnable = 1;
constexpr uint8_t kFifoControl = 2;
constexpr uint8_t kLineControl = 3;
constexpr uint8_t kModemControl = 4;
constexpr uint8_t kLineStatus = 5;
constexpr uint8_t kTransmitReady = 0x20;
constexpr uint32_t kPollLimit = 1000000;

uint16_t g_base_port = 0x3f8;
bool g_initialized = false;

void out8(uint16_t port, uint8_t value) {
    __asm__ volatile("outb %0, %1" : : "a"(value), "Nd"(port));
}

uint8_t in8(uint16_t port) {
    uint8_t value;
    __asm__ volatile("inb %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}
}

bool initialize(uint16_t base_port) {
    if (base_port == 0) return false;
    g_base_port = base_port;
    out8(g_base_port + kInterruptEnable, 0x00);
    out8(g_base_port + kLineControl, 0x80); // Enable divisor latch.
    out8(g_base_port + kData, 0x01);        // Divisor 1: 115200 baud.
    out8(g_base_port + kInterruptEnable, 0x00);
    out8(g_base_port + kLineControl, 0x03); // 8 data bits, no parity, one stop.
    out8(g_base_port + kFifoControl, 0xc7);
    out8(g_base_port + kModemControl, 0x0b);
    g_initialized = true;
    return true;
}

bool write_byte(uint8_t byte) {
    if (!g_initialized) return false;
    for (uint32_t attempt = 0; attempt < kPollLimit; ++attempt) {
        const uint8_t status = in8(g_base_port + kLineStatus);
        if (status == 0xff) return false;
        if ((status & kTransmitReady) != 0) {
            out8(g_base_port + kData, byte);
            return true;
        }
        __asm__ volatile("pause");
    }
    return false;
}

bool write(const char* text) {
    if (text == nullptr || !g_initialized) return false;
    while (*text != '\0') {
        if (!write_byte(static_cast<uint8_t>(*text++))) return false;
    }
    return true;
}

} // namespace annyos::serial
