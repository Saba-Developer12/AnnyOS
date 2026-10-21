#pragma once

#include <stdint.h>

namespace annyos::api {

constexpr uint32_t kVersion = 1;

enum class Service : uint8_t {
    version = 1,
    date = 2,
    time = 3,
    memory = 4,
    reboot = 5,
};

using ConsoleWriteLine = bool (*)(void* context, const char* text, uint8_t length);
using ConsoleWriteTitle = bool (*)(void* context, const char* text, uint8_t length);
using ConsoleWriteHelpRow = bool (*)(void* context, const char* command, uint8_t command_length,
                                     const char* description, uint8_t description_length);
using InvokeService = bool (*)(void* context, Service service);

// Stable host table presented to version 1 .AP bytecode applications.
struct AnnyOsApiV1 {
    // Hosts may append fields in later versions; the V1 prefix remains stable.
    uint32_t structure_size;
    uint32_t version;
    void* context;
    ConsoleWriteLine console_write_line;
    ConsoleWriteTitle console_write_title;
    ConsoleWriteHelpRow console_write_help_row;
    InvokeService invoke_service;
};

} // namespace annyos::api
