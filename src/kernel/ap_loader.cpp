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

ApResult process_program(const uint8_t* code, uint32_t code_size,
                         const annyos::api::AnnyOsApiV1* api,
                         const ApArguments* arguments, uint32_t declared_api_version,
                         bool execute,
                         uint32_t* processed_instructions) {
    uint32_t offset = 0;
    uint32_t instruction_count = 0;
    while (offset < code_size && instruction_count < kApMaximumInstructions) {
        ++instruction_count;
        const ApOpcode opcode = static_cast<ApOpcode>(code[offset++]);
        if (opcode == ApOpcode::exit) {
            if (offset != code_size) return ApResult::invalid_program;
            if (processed_instructions != nullptr) *processed_instructions = instruction_count;
            return ApResult::success;
        }
        if (opcode == ApOpcode::api_call) {
            if (offset >= code_size) return ApResult::invalid_program;
            const uint8_t service = code[offset++];
            if (service < static_cast<uint8_t>(annyos::api::Service::version) ||
                service > static_cast<uint8_t>(annyos::api::Service::time_get_rtc)) {
                return ApResult::invalid_program;
            }
            const uint32_t service_api_version = service >=
                static_cast<uint8_t>(annyos::api::Service::sys_shutdown) ? 3u :
                (service >= static_cast<uint8_t>(annyos::api::Service::config_list) ? 2u : 1u);
            if (declared_api_version < service_api_version) return ApResult::invalid_program;
            if (execute) {
                const auto identifier = static_cast<annyos::api::Service>(service);
                const bool invoked = service >= static_cast<uint8_t>(annyos::api::Service::sys_shutdown)
                    ? api->invoke_service_v3 != nullptr &&
                      api->invoke_service_v3(api->context, identifier, arguments)
                    : api->invoke_service(api->context, identifier);
                if (!invoked) return ApResult::output_failed;
            }
            continue;
        }

        if (opcode == ApOpcode::print_argument) {
            if (offset >= code_size || code[offset] >= kApMaximumArguments) {
                return ApResult::invalid_program;
            }
            const uint8_t index = code[offset++];
            if (execute) {
                const char* text = arguments != nullptr && index < arguments->count
                    ? arguments->values[index] : "";
                uint32_t length = 0;
                while (length < kApMaximumArgumentBytes && text[length] != '\0') ++length;
                if (!api->console_write_line(api->context, text, static_cast<uint8_t>(length))) {
                    return ApResult::output_failed;
                }
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
            if (execute) {
                char text[kApMaximumPrintBytes + 1];
                for (uint32_t index = 0; index < text_length; ++index) {
                    text[index] = static_cast<char>(code[offset + index]);
                }
                text[text_length] = '\0';
                const bool written = opcode == ApOpcode::title
                    ? api->console_write_title(api->context, text, static_cast<uint8_t>(text_length))
                    : api->console_write_line(api->context, text, static_cast<uint8_t>(text_length));
                if (!written) return ApResult::output_failed;
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
            if (execute) {
                char command[17];
                char description[79];
                for (uint32_t index = 0; index < command_length; ++index) {
                    command[index] = static_cast<char>(code[offset + index]);
                }
                command[command_length] = '\0';
                for (uint32_t index = 0; index < description_length; ++index) {
                    description[index] = static_cast<char>(code[offset + command_length + index]);
                }
                description[description_length] = '\0';
                if (!api->console_write_help_row(api->context, command,
                        static_cast<uint8_t>(command_length), description,
                        static_cast<uint8_t>(description_length))) return ApResult::output_failed;
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
                    const ApArguments* arguments, uint32_t* executed_instructions) {
    if (executed_instructions != nullptr) *executed_instructions = 0;
    if (image == nullptr || host.api == nullptr || image_size < kApHeaderBytes ||
        image_size > kApMaximumFileBytes) return ApResult::invalid_argument;
    const annyos::api::AnnyOsApiV1& api = *host.api;
    constexpr uint32_t kV2ApiSize = offsetof(annyos::api::AnnyOsApiV1, invoke_service_v3);
    if (api.structure_size < kV2ApiSize || api.version < 1 ||
        api.console_write_line == nullptr ||
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
        read_u32(image + 28) != 0) {
        return ApResult::invalid_header;
    }
    const uint32_t required_api_version = read_u32(image + 36);
    if (required_api_version == 0 || required_api_version > api.version ||
        (required_api_version >= 3 &&
         (api.structure_size < sizeof(annyos::api::AnnyOsApiV1) ||
          api.invoke_service_v3 == nullptr))) {
        return ApResult::unsupported_version;
    }

    const uint32_t code_size = read_u32(image + 24);
    const uint32_t expected_checksum = read_u32(image + 32);
    if (code_size == 0 || code_size != image_size - kApHeaderBytes) return ApResult::invalid_header;
    const uint8_t* code = image + kApHeaderBytes;
    if (checksum(code, code_size) != expected_checksum) return ApResult::invalid_checksum;

    if (arguments != nullptr && arguments->count > kApMaximumArguments) {
        return ApResult::invalid_argument;
    }
    const ApResult validation = process_program(code, code_size, &api, arguments,
                                                required_api_version, false, nullptr);
    if (validation != ApResult::success) return validation;
    return process_program(code, code_size, &api, arguments, required_api_version,
                           true, executed_instructions);
}

} // namespace annyos::application
