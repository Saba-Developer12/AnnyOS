#include "shell_state.hpp"

namespace annyos::shell {

bool equal_ignore_case(const char* left, const char* right) {
    if (left == nullptr || right == nullptr) return false;
    while (*left != '\0' && *right != '\0') {
        if (lowercase(*left) != lowercase(*right)) return false;
        ++left;
        ++right;
    }
    return *left == '\0' && *right == '\0';
}

char lowercase(char c) {
    return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
}

char uppercase(char c) {
    return c >= 'a' && c <= 'z' ? static_cast<char>(c - 'a' + 'A') : c;
}

bool command_equals(const char* input, const char* command) {
    if (input == nullptr || command == nullptr) return false;
    uint32_t i = 0;
    while (input[i] != '\0' && command[i] != '\0') {
        if (lowercase(input[i]) != command[i]) return false;
        ++i;
    }
    return input[i] == '\0' && command[i] == '\0';
}

bool command_starts_with(const char* input, const char* prefix) {
    if (input == nullptr || prefix == nullptr) return false;
    uint32_t i = 0;
    while (prefix[i] != '\0') {
        if (input[i] == '\0' || lowercase(input[i]) != prefix[i]) return false;
        ++i;
    }
    return true;
}

void append_decimal(char* output, uint32_t* length, uint64_t value) {
    if (output == nullptr || length == nullptr) return;
    char digits[20];
    uint32_t count = 0;
    do {
        digits[count++] = static_cast<char>('0' + value % 10);
        value /= 10;
    } while (value != 0 && count < sizeof(digits));
    while (count > 0) output[(*length)++] = digits[--count];
    output[*length] = '\0';
}

void remember_command(ConsoleState& console) {
    if (console.input_length == 0) return;
    const uint32_t last = console.command_history_count == 0 ? 0 : console.command_history_count - 1;
    if (console.command_history_count == 0 ||
        !equal_ignore_case(console.command_history[last], console.input)) {
        if (console.command_history_count == kHistoryCapacity) {
            for (uint32_t index = 1; index < kHistoryCapacity; ++index) {
                for (uint32_t character = 0; character < kLineCapacity; ++character) {
                    console.command_history[index - 1][character] = console.command_history[index][character];
                }
            }
            --console.command_history_count;
        }
        char* destination = console.command_history[console.command_history_count++];
        uint32_t index = 0;
        while (index + 1 < kLineCapacity && console.input[index] != '\0') {
            destination[index] = console.input[index];
            ++index;
        }
        destination[index] = '\0';
    }
    console.command_history_cursor = console.command_history_count;
}

void recall_command(ConsoleState& console, bool older) {
    if (older) {
        if (console.command_history_cursor > 0) --console.command_history_cursor;
    } else if (console.command_history_cursor < console.command_history_count) {
        ++console.command_history_cursor;
    }

    console.input_length = 0;
    if (console.command_history_cursor < console.command_history_count) {
        const char* source = console.command_history[console.command_history_cursor];
        while (console.input_length + 1 < kInputCapacity && source[console.input_length] != '\0') {
            console.input[console.input_length] = source[console.input_length];
            ++console.input_length;
        }
    }
    console.input[console.input_length] = '\0';
}

uint32_t append_text(char* destination, uint32_t length, uint32_t capacity, const char* text) {
    if (destination == nullptr || capacity == 0 || text == nullptr) return 0;
    if (length >= capacity) length = capacity - 1;
    while (length + 1 < capacity && *text != '\0') destination[length++] = *text++;
    destination[length] = '\0';
    return length;
}

uint32_t bounded_string_length(const char* text, uint32_t capacity) {
    if (text == nullptr) return 0;
    uint32_t length = 0;
    while (length < capacity && text[length] != '\0') ++length;
    return length;
}

bool parse_decimal(const char* text, uint32_t* value) {
    if (text == nullptr || value == nullptr || *text == '\0') return false;
    uint32_t parsed = 0;
    while (*text != '\0') {
        if (*text < '0' || *text > '9') return false;
        const uint32_t digit = static_cast<uint32_t>(*text++ - '0');
        if (parsed > (0xffffffffu - digit) / 10u) return false;
        parsed = parsed * 10u + digit;
    }
    *value = parsed;
    return true;
}


void add_history_line(ConsoleState& console, const char* text, Color color,
                      bool bold, uint8_t accent_length, bool columned) {
    if (text == nullptr) text = "";
    if (console.history_count == kHistoryCapacity) {
        for (uint32_t line = 1; line < kHistoryCapacity; ++line) {
            for (uint32_t i = 0; i < kLineCapacity; ++i) console.history[line - 1][i] = console.history[line][i];
            console.history_color[line - 1] = console.history_color[line];
            console.history_bold[line - 1] = console.history_bold[line];
            console.history_accent_length[line - 1] = console.history_accent_length[line];
            console.history_columned[line - 1] = console.history_columned[line];
            console.history_tabular[line - 1] = console.history_tabular[line];
        }
        --console.history_count;
    }
    const uint32_t line = console.history_count++;
    char* destination = console.history[line];
    uint32_t i = 0;
    while (i + 1 < kLineCapacity && text[i] != '\0') destination[i] = text[i], ++i;
    destination[i] = '\0';
    console.history_color[line] = color;
    console.history_bold[line] = bold;
    console.history_accent_length[line] = accent_length < i ? accent_length : static_cast<uint8_t>(i);
    console.history_columned[line] = columned;
    console.history_tabular[line] = false;
}


} // namespace annyos::shell
