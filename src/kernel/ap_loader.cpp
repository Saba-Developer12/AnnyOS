#include "annyos/ap_format.hpp"

namespace annyos::application {
namespace {
constexpr uint8_t kMagic[8] = {'A', 'N', 'N', 'Y', 'A', 'P', '1', 0};
constexpr uint32_t kFnvOffsetBasis = 2166136261u;
constexpr uint32_t kFnvPrime = 16777619u;

uint16_t read_u16(const uint8_t* bytes) {
    return static_cast<uint16_t>(bytes[0]) | (static_cast<uint16_t>(bytes[1]) << 8);
}

uint32_t read_u32(const uint8_t* bytes) {
    return static_cast<uint32_t>(bytes[0]) |
           (static_cast<uint32_t>(bytes[1]) << 8) |
           (static_cast<uint32_t>(bytes[2]) << 16) |
           (static_cast<uint32_t>(bytes[3]) << 24);
}

uint32_t checksum(const uint8_t* bytes, uint32_t length) {
    uint32_t value = kFnvOffsetBasis;
    for (uint32_t index = 0; index < length; ++index) {
        value ^= bytes[index];
        value *= kFnvPrime;
    }
    return value;
}

ApResult validate_program(const uint8_t* code, uint32_t code_size) {
    uint32_t offset = 0;
    uint32_t instruction_count = 0;
    while (offset < code_size && instruction_count < kApMaximumInstructions) {
        ++instruction_count;
        const ApOpcode opcode = static_cast<ApOpcode>(code[offset++]);
        if (opcode == ApOpcode::exit) {
            return offset == code_size ? ApResult::success : ApResult::invalid_program;
        }
        if (opcode == ApOpcode::api_call) {
            if (offset >= code_size) return ApResult::invalid_program;
            const uint8_t service = code[offset++];
            if (service < static_cast<uint8_t>(annyos::api::Service::version) ||
                service > static_cast<uint8_t>(annyos::api::Service::reboot)) {
                return ApResult::invalid_program;
            }
            continue;
        }

        if (opcode == ApOpcode::print || opcode == ApOpcode::title) {
            if (offset >= code_size) return ApResult::invalid_program;
            const uint32_t text_length = code[offset++];
            if (text_length > kApMaximumPrintBytes || text_length > code_size - offset) {
                return ApResult::invalid_program;
            }
            for (uint32_t index = 0; index < text_length; ++index) {
                const uint8_t character = code[offset + index];
                if (character < 32 || character > 126) return ApResult::invalid_program;
            }
            offset += text_length;
            continue;
        }

        if (opcode == ApOpcode::help_row) {
            if (code_size - offset < 2) return ApResult::invalid_program;
            const uint32_t command_length = code[offset++];
            const uint32_t description_length = code[offset++];
            if (command_length == 0 || command_length > 16 || description_length > 78 ||
                command_length + description_length > code_size - offset) {
                return ApResult::invalid_program;
            }
            for (uint32_t index = 0; index < command_length + description_length; ++index) {
                const uint8_t character = code[offset + index];
                if (character < 32 || character > 126) return ApResult::invalid_program;
            }
            offset += command_length + description_length;
            continue;
        }
        return ApResult::invalid_program;
    }
    return ApResult::invalid_program;
}
}

ApResult execute_ap(const uint8_t* image, uint32_t image_size, const ApHost& host,
                    uint32_t* executed_instructions) {
    if (executed_instructions != nullptr) *executed_instructions = 0;
    if (image == nullptr || host.api == nullptr || image_size < kApHeaderBytes ||
        image_size > kApMaximumFileBytes) return ApResult::invalid_argument;
    const annyos::api::AnnyOsApiV1& api = *host.api;
    if (api.structure_size < sizeof(annyos::api::AnnyOsApiV1) ||
        api.version != annyos::api::kVersion || api.console_write_line == nullptr ||
        api.console_write_title == nullptr ||
        api.console_write_help_row == nullptr || api.invoke_service == nullptr) {
        return ApResult::invalid_argument;
    }

    for (uint32_t index = 0; index < sizeof(kMagic); ++index) {
        if (image[index] != kMagic[index]) return ApResult::invalid_header;
    }
    const uint16_t version = read_u16(image + 8);
    const uint16_t header_size = read_u16(image + 10);
    if (version != kApVersion) return ApResult::unsupported_version;
    if (header_size != kApHeaderBytes || read_u32(image + 12) != 0 ||
        read_u32(image + 16) != image_size || read_u32(image + 20) != kApHeaderBytes ||
        read_u32(image + 28) != 0 || read_u32(image + 36) != api.version) {
        return ApResult::invalid_header;
    }

    const uint32_t code_size = read_u32(image + 24);
    const uint32_t expected_checksum = read_u32(image + 32);
    if (code_size == 0 || code_size != image_size - kApHeaderBytes) return ApResult::invalid_header;
    const uint8_t* code = image + kApHeaderBytes;
    if (checksum(code, code_size) != expected_checksum) return ApResult::invalid_checksum;

    const ApResult validation = validate_program(code, code_size);
    if (validation != ApResult::success) return validation;

    uint32_t offset = 0;
    uint32_t instruction_count = 0;
    while (offset < code_size) {
        ++instruction_count;
        const ApOpcode opcode = static_cast<ApOpcode>(code[offset++]);
        if (opcode == ApOpcode::exit) break;
        if (opcode == ApOpcode::api_call) {
            const auto service = static_cast<annyos::api::Service>(code[offset++]);
            if (!api.invoke_service(api.context, service)) return ApResult::output_failed;
        } else if (opcode == ApOpcode::print || opcode == ApOpcode::title) {
            const uint8_t text_length = code[offset++];
            char line[kApMaximumPrintBytes + 1];
            for (uint32_t index = 0; index < text_length; ++index) {
                line[index] = static_cast<char>(code[offset + index]);
            }
            line[text_length] = '\0';
            offset += text_length;
            const bool written = opcode == ApOpcode::title
                ? api.console_write_title(api.context, line, text_length)
                : api.console_write_line(api.context, line, text_length);
            if (!written) return ApResult::output_failed;
        } else if (opcode == ApOpcode::help_row) {
            const uint8_t command_length = code[offset++];
            const uint8_t description_length = code[offset++];
            char command[17];
            char description[79];
            for (uint32_t index = 0; index < command_length; ++index) {
                command[index] = static_cast<char>(code[offset + index]);
            }
            command[command_length] = '\0';
            offset += command_length;
            for (uint32_t index = 0; index < description_length; ++index) {
                description[index] = static_cast<char>(code[offset + index]);
            }
            description[description_length] = '\0';
            offset += description_length;
            if (!api.console_write_help_row(api.context, command, command_length,
                                            description, description_length)) {
                return ApResult::output_failed;
            }
        }
    }
    if (executed_instructions != nullptr) *executed_instructions = instruction_count;
    return ApResult::success;
}

} // namespace annyos::application
