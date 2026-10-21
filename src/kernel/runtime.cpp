#include <stdint.h>

extern "C" void* memset(void* destination, int value, __SIZE_TYPE__ count) {
    auto* bytes = static_cast<uint8_t*>(destination);
    for (__SIZE_TYPE__ index = 0; index < count; ++index) {
        bytes[index] = static_cast<uint8_t>(value);
    }
    return destination;
}

extern "C" void* memcpy(void* destination, const void* source, __SIZE_TYPE__ count) {
    auto* output = static_cast<uint8_t*>(destination);
    const auto* input = static_cast<const uint8_t*>(source);
    for (__SIZE_TYPE__ index = 0; index < count; ++index) output[index] = input[index];
    return destination;
}
