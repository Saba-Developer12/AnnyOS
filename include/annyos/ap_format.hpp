#pragma once

#include <stdint.h>
#include "annyos/api.hpp"

namespace annyos::application {

constexpr uint32_t kApVersion = 1;
constexpr uint32_t kApHeaderBytes = 40;
constexpr uint32_t kApMaximumFileBytes = 64 * 1024;
constexpr uint32_t kApMaximumInstructions = 4096;
constexpr uint32_t kApMaximumPrintBytes = 95;

#pragma pack(push, 1)
struct ApHeader {
    char magic[8];             // "ANNYAP1\0"
    uint16_t version;          // kApVersion
    uint16_t header_size;      // kApHeaderBytes
    uint32_t flags;            // must be zero in version 1
    uint32_t file_size;        // exact byte length of the file
    uint32_t code_offset;      // kApHeaderBytes
    uint32_t code_size;
    uint32_t entry_offset;     // zero in version 1
    uint32_t code_checksum;    // FNV-1a 32-bit over code_size bytes
    uint32_t api_version;      // annyos::api::kVersion
};
#pragma pack(pop)

static_assert(sizeof(ApHeader) == kApHeaderBytes, "AP header layout changed");

enum class ApOpcode : uint8_t {
    print = 0x01, // u8 byte count, then printable ASCII bytes
    api_call = 0x02, // u8 annyos::api::Service identifier
    help_row = 0x03, // u8 command length, u8 description length, then both strings
    title = 0x04, // u8 byte count, then printable ASCII bytes
    exit = 0xff,  // must be the final instruction
};

enum class ApResult : uint8_t {
    success,
    invalid_argument,
    invalid_header,
    unsupported_version,
    invalid_checksum,
    invalid_program,
    output_failed,
};

struct ApHost {
    const annyos::api::AnnyOsApiV1* api;
};

// Version 1 is a bounded, interpreted bytecode format. It intentionally does
// not execute native ring-0 machine code while AnnyOS has no user mode or MMU
// isolation. Output instructions each produce one console line.
ApResult execute_ap(const uint8_t* image, uint32_t image_size, const ApHost& host,
                    uint32_t* executed_instructions = nullptr);

} // namespace annyos::application
