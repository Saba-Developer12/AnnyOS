#include <stdint.h>
#include <stdarg.h>

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

extern "C" void* memmove(void* destination, const void* source, __SIZE_TYPE__ count) {
    auto* output = static_cast<uint8_t*>(destination);
    const auto* input = static_cast<const uint8_t*>(source);
    if (output == input || count == 0) return destination;
    if (reinterpret_cast<uintptr_t>(output) < reinterpret_cast<uintptr_t>(input)) {
        for (__SIZE_TYPE__ index = 0; index < count; ++index) output[index] = input[index];
    } else {
        while (count != 0) {
            --count;
            output[count] = input[count];
        }
    }
    return destination;
}

extern "C" int memcmp(const void* left, const void* right, __SIZE_TYPE__ count) {
    const auto* a = static_cast<const uint8_t*>(left);
    const auto* b = static_cast<const uint8_t*>(right);
    for (__SIZE_TYPE__ index = 0; index < count; ++index) {
        if (a[index] != b[index]) return a[index] < b[index] ? -1 : 1;
    }
    return 0;
}

extern "C" __SIZE_TYPE__ strlen(const char* text) {
    __SIZE_TYPE__ length = 0;
    while (text[length] != '\0') ++length;
    return length;
}

extern "C" int strcmp(const char* left, const char* right) {
    while (*left != '\0' && *left == *right) {
        ++left;
        ++right;
    }
    const uint8_t a = static_cast<uint8_t>(*left);
    const uint8_t b = static_cast<uint8_t>(*right);
    return a == b ? 0 : (a < b ? -1 : 1);
}

extern "C" int strncmp(const char* left, const char* right, __SIZE_TYPE__ count) {
    while (count != 0 && *left != '\0' && *left == *right) {
        ++left;
        ++right;
        --count;
    }
    if (count == 0) return 0;
    const uint8_t a = static_cast<uint8_t>(*left);
    const uint8_t b = static_cast<uint8_t>(*right);
    return a == b ? 0 : (a < b ? -1 : 1);
}

extern "C" char* strchr(const char* text, int character) {
    const char sought = static_cast<char>(character);
    for (;;) {
        if (*text == sought) return const_cast<char*>(text);
        if (*text == '\0') return nullptr;
        ++text;
    }
}

extern "C" char* strncpy(char* destination, const char* source, __SIZE_TYPE__ count) {
    __SIZE_TYPE__ index = 0;
    while (index < count && source[index] != '\0') {
        destination[index] = source[index];
        ++index;
    }
    while (index < count) destination[index++] = '\0';
    return destination;
}

extern "C" int atoi(const char* text) {
    while (*text == ' ' || (*text >= '\t' && *text <= '\r')) ++text;
    bool negative = false;
    if (*text == '+' || *text == '-') negative = *text++ == '-';
    uint32_t value = 0;
    const uint32_t limit = negative ? 0x80000000u : 0x7fffffffu;
    while (*text >= '0' && *text <= '9') {
        const uint32_t digit = static_cast<uint32_t>(*text++ - '0');
        if (value > (limit - digit) / 10) return negative ? (-2147483647 - 1) : 2147483647;
        value = value * 10 + digit;
    }
    if (!negative) return static_cast<int>(value);
    if (value == 0x80000000u) return -2147483647 - 1;
    return -static_cast<int>(value);
}

namespace {

struct FormatWriter {
    char* output;
    __SIZE_TYPE__ capacity;
    __SIZE_TYPE__ length;
};

void write_format_char(FormatWriter& writer, char value) {
    if (writer.capacity != 0 && writer.length + 1 < writer.capacity)
        writer.output[writer.length] = value;
    if (writer.length < static_cast<__SIZE_TYPE__>(0x7fffffff)) ++writer.length;
}

void write_format_string(FormatWriter& writer, const char* value) {
    if (value == nullptr) value = "(null)";
    while (*value != '\0') write_format_char(writer, *value++);
}

void write_format_unsigned(FormatWriter& writer, uint32_t value, uint32_t base,
                           bool uppercase_digits = false) {
    char digits[32];
    uint32_t count = 0;
    do {
        const uint32_t digit = value % base;
        digits[count++] = static_cast<char>(digit < 10 ? '0' + digit :
            (uppercase_digits ? 'A' : 'a') + digit - 10);
        value /= base;
    } while (value != 0 && count < sizeof(digits));
    while (count != 0) write_format_char(writer, digits[--count]);
}

} // namespace

extern "C" int snprintf(char* output, __SIZE_TYPE__ capacity, const char* format, ...) {
    if (output == nullptr && capacity != 0) return -1;
    FormatWriter writer{output, capacity, 0};
    va_list arguments;
    va_start(arguments, format);
    while (*format != '\0') {
        if (*format++ != '%') {
            write_format_char(writer, format[-1]);
            continue;
        }
        if (*format == '%') {
            ++format;
            write_format_char(writer, '%');
            continue;
        }
        bool long_value = false;
        if (*format == 'l') {
            long_value = true;
            ++format;
            if (*format == 'l') ++format;
        }
        const char specifier = *format;
        if (specifier != '\0') ++format;
        if (specifier == 's') {
            write_format_string(writer, va_arg(arguments, const char*));
        } else if (specifier == 'c') {
            write_format_char(writer, static_cast<char>(va_arg(arguments, int)));
        } else if (specifier == 'd' || specifier == 'i') {
            const int32_t signed_value = long_value
                ? static_cast<int32_t>(va_arg(arguments, long long))
                : static_cast<int32_t>(va_arg(arguments, int));
            const bool negative = signed_value < 0;
            if (negative) write_format_char(writer, '-');
            const uint32_t magnitude = negative
                ? 0u - static_cast<uint32_t>(signed_value)
                : static_cast<uint32_t>(signed_value);
            write_format_unsigned(writer, magnitude, 10);
        } else if (specifier == 'u' || specifier == 'x' || specifier == 'X') {
            const uint32_t value = long_value
                ? static_cast<uint32_t>(va_arg(arguments, unsigned long long))
                : static_cast<uint32_t>(va_arg(arguments, unsigned int));
            write_format_unsigned(writer, value, specifier == 'u' ? 10 : 16,
                                  specifier == 'X');
        } else if (specifier == 'p') {
            write_format_string(writer, "0x");
            write_format_unsigned(writer, static_cast<uint32_t>(
                reinterpret_cast<uintptr_t>(va_arg(arguments, void*))), 16);
        } else {
            write_format_char(writer, '%');
            if (specifier != '\0') write_format_char(writer, specifier);
        }
    }
    va_end(arguments);
    if (capacity != 0) output[writer.length < capacity ? writer.length : capacity - 1] = '\0';
    return static_cast<int>(writer.length);
}
