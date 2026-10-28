#include "annyos/curl_host_bridge.hpp"

#include "annyos/timer_pit.hpp"

namespace annyos::curl {
namespace {

constexpr uint16_t kCom2 = 0x2f8;
constexpr uint16_t kData = 0;
constexpr uint16_t kInterruptEnable = 1;
constexpr uint16_t kFifoControl = 2;
constexpr uint16_t kLineControl = 3;
constexpr uint16_t kModemControl = 4;
constexpr uint16_t kLineStatus = 5;
constexpr uint8_t kReceiveReady = 0x01;
constexpr uint8_t kTransmitReady = 0x20;
constexpr uint64_t kTransferTimeoutMs = 30000;
bool g_uart_initialized;

void out8(uint16_t port, uint8_t value) {
    __asm__ volatile("outb %0, %1" : : "a"(value), "Nd"(port));
}

uint8_t in8(uint16_t port) {
    uint8_t value;
    __asm__ volatile("inb %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

bool initialize_uart() {
    if (g_uart_initialized) return true;
    if (in8(kCom2 + kLineStatus) == 0xff) return false;
    out8(kCom2 + kInterruptEnable, 0x00);
    out8(kCom2 + kLineControl, 0x80);
    out8(kCom2 + kData, 0x01);
    out8(kCom2 + kInterruptEnable, 0x00);
    out8(kCom2 + kLineControl, 0x03);
    out8(kCom2 + kFifoControl, 0xc7);
    out8(kCom2 + kModemControl, 0x0b);
    g_uart_initialized = true;
    return true;
}

bool write_byte(uint8_t byte, uint64_t deadline) {
    while (annyos::timer::ticks() < deadline) {
        const uint8_t status = in8(kCom2 + kLineStatus);
        if (status == 0xff) return false;
        if ((status & kTransmitReady) != 0) {
            out8(kCom2 + kData, byte);
            return true;
        }
        __asm__ volatile("pause");
    }
    return false;
}

bool read_byte(uint8_t* byte, uint64_t deadline) {
    if (byte == nullptr) return false;
    while (annyos::timer::ticks() < deadline) {
        const uint8_t status = in8(kCom2 + kLineStatus);
        if (status == 0xff) return false;
        if ((status & kReceiveReady) != 0) {
            *byte = in8(kCom2 + kData);
            return true;
        }
        __asm__ volatile("pause");
    }
    return false;
}

bool read_line(char* output, uint32_t capacity, uint64_t deadline) {
    if (output == nullptr || capacity < 2) return false;
    uint32_t length = 0;
    while (length + 1 < capacity) {
        uint8_t byte;
        if (!read_byte(&byte, deadline)) return false;
        if (byte == '\n') {
            output[length] = '\0';
            return true;
        }
        if (byte != '\r' && byte >= 32 && byte <= 126) output[length++] = static_cast<char>(byte);
    }
    output[length] = '\0';
    return false;
}

bool parse_unsigned(const char** cursor, uint32_t* value) {
    if (cursor == nullptr || *cursor == nullptr || value == nullptr ||
        **cursor < '0' || **cursor > '9') return false;
    uint32_t parsed = 0;
    while (**cursor >= '0' && **cursor <= '9') {
        const uint32_t digit = static_cast<uint32_t>(*(*cursor)++ - '0');
        if (parsed > (0xffffffffu - digit) / 10u) return false;
        parsed = parsed * 10u + digit;
    }
    *value = parsed;
    return true;
}

bool parse_response_header(const char* line, uint8_t* exit_code,
                           uint32_t* body_length, uint32_t* diagnostic_length,
                           bool* truncated) {
    if (line == nullptr || exit_code == nullptr || body_length == nullptr ||
        diagnostic_length == nullptr || truncated == nullptr ||
        line[0] != 'R' || line[1] != ' ') return false;
    const char* cursor = line + 2;
    uint32_t parsed_exit = 0;
    uint32_t parsed_body_length = 0;
    uint32_t parsed_diagnostic_length = 0;
    uint32_t parsed_truncated = 0;
    if (!parse_unsigned(&cursor, &parsed_exit) || *cursor++ != ' ' ||
        !parse_unsigned(&cursor, &parsed_body_length) || *cursor++ != ' ' ||
        !parse_unsigned(&cursor, &parsed_diagnostic_length) || *cursor++ != ' ' ||
        !parse_unsigned(&cursor, &parsed_truncated) || *cursor != '\0' ||
        parsed_exit > 255 || parsed_body_length > kMaximumResponseBytes ||
        parsed_diagnostic_length > kMaximumDiagnosticBytes || parsed_truncated > 1) {
        return false;
    }
    *exit_code = static_cast<uint8_t>(parsed_exit);
    *body_length = parsed_body_length;
    *diagnostic_length = parsed_diagnostic_length;
    *truncated = parsed_truncated != 0;
    return true;
}

} // namespace

bool fetch_from_host(const char* arguments, HostResponse* response) {
    if (arguments == nullptr || response == nullptr || !initialize_uart()) return false;
    response->body_length = 0;
    response->diagnostic_length = 0;
    response->curl_exit_code = 0;
    response->truncated = false;

    uint32_t argument_length = 0;
    while (arguments[argument_length] != '\0' && argument_length < 500) {
        const char byte = arguments[argument_length];
        if (byte < 32 || byte > 126 || byte == '\r' || byte == '\n') return false;
        ++argument_length;
    }
    if (argument_length == 0 || arguments[argument_length] != '\0') return false;

    const uint64_t deadline = annyos::timer::ticks() + kTransferTimeoutMs;
    const char prefix[] = "CURL ";
    for (uint32_t index = 0; prefix[index] != '\0'; ++index) {
        if (!write_byte(static_cast<uint8_t>(prefix[index]), deadline)) return false;
    }
    for (uint32_t index = 0; index < argument_length; ++index) {
        if (!write_byte(static_cast<uint8_t>(arguments[index]), deadline)) return false;
    }
    if (!write_byte('\n', deadline)) return false;

    char header[64];
    if (!read_line(header, sizeof(header), deadline) ||
        !parse_response_header(header, &response->curl_exit_code,
                               &response->body_length, &response->diagnostic_length,
                               &response->truncated)) return false;
    for (uint32_t index = 0; index < response->diagnostic_length; ++index) {
        if (!read_byte(&response->diagnostic[index], deadline)) return false;
    }
    for (uint32_t index = 0; index < response->body_length; ++index) {
        if (!read_byte(&response->body[index], deadline)) return false;
    }
    return true;
}

} // namespace annyos::curl
