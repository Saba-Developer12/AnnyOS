#include "annyos/boot_info.hpp"
#include "annyos/api.hpp"
#include "annyos/ata_pio.hpp"
#include "annyos/ap_format.hpp"
#include "annyos/fat32.hpp"
#include "annyos/font_data.hpp"
#include "annyos/ps2_keyboard.hpp"

namespace {

struct Color { uint8_t r, g, b; };

constexpr Color kDefaultScreenColor{0, 0, 170};
constexpr Color kDefaultTextColor{220, 220, 220};
constexpr Color kDefaultPromptColor{85, 255, 255};
constexpr Color kInputColor{255, 255, 255};
constexpr Color kHelpTitleColor{255, 220, 120};
constexpr int32_t kHelpDescriptionOffset = 128;
constexpr int32_t kLineHeight = 22;
constexpr int32_t kMargin = 28;
constexpr uint32_t kHistoryCapacity = 32;
constexpr uint32_t kLineCapacity = 100;
constexpr uint32_t kInputCapacity = 88;
constexpr uint32_t kWorkingDirectoryDepth = 12;
constexpr uint32_t kWorkingDirectoryPathCapacity = 80;
constexpr uint32_t kApBufferCapacity = annyos::application::kApMaximumFileBytes;

struct ConsoleState {
    char history[kHistoryCapacity][kLineCapacity];
    Color history_color[kHistoryCapacity];
    uint8_t history_accent_length[kHistoryCapacity];
    bool history_bold[kHistoryCapacity];
    bool history_columned[kHistoryCapacity];
    uint32_t history_count;
    char input[kInputCapacity];
    uint32_t input_length;
};

struct WorkingDirectory {
    uint32_t cluster_stack[kWorkingDirectoryDepth + 1];
    uint32_t depth;
    char path[kWorkingDirectoryPathCapacity];
};

struct KernelApiContext {
    ConsoleState* console;
    const annyos::BootInfo* boot_info;
    const annyos::FramebufferInfo* framebuffer;
    const WorkingDirectory* working_directory;
};

ConsoleState g_console;
WorkingDirectory g_working_directory;
uint8_t g_ap_image[kApBufferCapacity];
Color g_screen_color = kDefaultScreenColor;
Color g_text_color = kDefaultTextColor;
Color g_prompt_color = kDefaultPromptColor;

void debug_write(const char* text) {
    while (*text != '\0') {
        const uint8_t byte = static_cast<uint8_t>(*text++);
        __asm__ volatile("outb %0, $0xe9" : : "a"(byte));
    }
}

uint32_t pack_color(Color color, const annyos::FramebufferInfo& fb) {
    if (fb.format == annyos::PixelFormat::rgb_reserved_8) {
        return static_cast<uint32_t>(color.r) |
               (static_cast<uint32_t>(color.g) << 8) |
               (static_cast<uint32_t>(color.b) << 16);
    }
    if (fb.format == annyos::PixelFormat::bgr_reserved_8) {
        return static_cast<uint32_t>(color.b) |
               (static_cast<uint32_t>(color.g) << 8) |
               (static_cast<uint32_t>(color.r) << 16);
    }
    return ((static_cast<uint32_t>(color.r) << __builtin_ctz(fb.mask.red)) & fb.mask.red) |
           ((static_cast<uint32_t>(color.g) << __builtin_ctz(fb.mask.green)) & fb.mask.green) |
           ((static_cast<uint32_t>(color.b) << __builtin_ctz(fb.mask.blue)) & fb.mask.blue) |
           fb.mask.reserved;
}

void fill_rect_packed(const annyos::FramebufferInfo& fb, int32_t x, int32_t y,
                      int32_t width, int32_t height, uint32_t packed_color) {
    if (width <= 0 || height <= 0) return;
    const int32_t x0 = x < 0 ? 0 : x;
    const int32_t y0 = y < 0 ? 0 : y;
    const int32_t x1 = x + width > static_cast<int32_t>(fb.width) ? static_cast<int32_t>(fb.width) : x + width;
    const int32_t y1 = y + height > static_cast<int32_t>(fb.height) ? static_cast<int32_t>(fb.height) : y + height;
    for (int32_t py = y0; py < y1; ++py) {
        volatile uint32_t* row = fb.address + static_cast<uint64_t>(py) * fb.pixels_per_scanline;
        for (int32_t px = x0; px < x1; ++px) row[px] = packed_color;
    }
}

void fill_rect(const annyos::FramebufferInfo& fb, int32_t x, int32_t y,
               int32_t width, int32_t height, Color color) {
    fill_rect_packed(fb, x, y, width, height, pack_color(color, fb));
}

const annyos::ui::Glyph& get_glyph(char character, bool bold = false) {
    if (character < 32 || character > 126) character = '?';
    const unsigned index = static_cast<unsigned>(character - 32);
    return bold ? annyos::ui::kSegoeUiBoldGlyphs[index] : annyos::ui::kSegoeUiGlyphs[index];
}

int32_t measure_text(const char* text) {
    int32_t width = 0;
    while (*text != '\0') width += get_glyph(*text++).advance;
    return width;
}

int32_t draw_text(const annyos::FramebufferInfo& fb, int32_t x, int32_t y,
                  const char* text, Color color, bool bold = false) {
    const uint32_t packed = pack_color(color, fb);
    int32_t pen_x = x;
    while (*text != '\0') {
        const annyos::ui::Glyph& glyph = get_glyph(*text++, bold);
        const int32_t glyph_x = pen_x + glyph.left;
        const int32_t glyph_y = y + annyos::ui::kSegoeUiBaseline - glyph.top;
        for (uint32_t row = 0; row < glyph.height; ++row) {
            for (uint32_t column = 0; column < glyph.width; ++column) {
                if ((glyph.rows[row] & (1u << (glyph.width - 1 - column))) != 0) {
                    fill_rect_packed(fb, glyph_x + static_cast<int32_t>(column),
                                     glyph_y + static_cast<int32_t>(row), 1, 1, packed);
                }
            }
        }
        pen_x += glyph.advance;
    }
    return pen_x;
}

void add_history_line(ConsoleState& console, const char* text, Color color = kDefaultTextColor,
                      bool bold = false, uint8_t accent_length = 0, bool columned = false) {
    if (console.history_count == kHistoryCapacity) {
        for (uint32_t line = 1; line < kHistoryCapacity; ++line) {
            for (uint32_t i = 0; i < kLineCapacity; ++i) console.history[line - 1][i] = console.history[line][i];
            console.history_color[line - 1] = console.history_color[line];
            console.history_bold[line - 1] = console.history_bold[line];
            console.history_accent_length[line - 1] = console.history_accent_length[line];
            console.history_columned[line - 1] = console.history_columned[line];
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
}

void clear_history(ConsoleState& console) {
    console.history_count = 0;
    console.history[0][0] = '\0';
}

char lowercase(char c);

uint32_t append_text(char* destination, uint32_t length, uint32_t capacity, const char* text) {
    while (length + 1 < capacity && *text != '\0') destination[length++] = *text++;
    destination[length] = '\0';
    return length;
}

void add_help_row(ConsoleState& console, const char* command, const char* description) {
    char line[kLineCapacity];
    uint32_t length = append_text(line, 0, sizeof(line), command);
    const uint32_t accent_length = length;
    while (length < 16) line[length++] = ' ';
    line[length] = '\0';
    append_text(line, length, sizeof(line), description);
    add_history_line(console, line, kDefaultTextColor, false, static_cast<uint8_t>(accent_length), true);
}

bool equal_ignore_case(const char* left, const char* right) {
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
    uint32_t i = 0;
    while (input[i] != '\0' && command[i] != '\0') {
        if (lowercase(input[i]) != command[i]) return false;
        ++i;
    }
    return input[i] == '\0' && command[i] == '\0';
}

bool command_starts_with(const char* input, const char* prefix) {
    uint32_t i = 0;
    while (prefix[i] != '\0') {
        if (input[i] == '\0' || lowercase(input[i]) != prefix[i]) return false;
        ++i;
    }
    return true;
}

void append_decimal(char* output, uint32_t* length, uint64_t value) {
    char digits[20];
    uint32_t count = 0;
    do {
        digits[count++] = static_cast<char>('0' + value % 10);
        value /= 10;
    } while (value != 0 && count < sizeof(digits));
    while (count > 0) output[(*length)++] = digits[--count];
    output[*length] = '\0';
}

uint8_t read_cmos(uint8_t reg) {
    __asm__ volatile("outb %0, $0x70" : : "a"(reg));
    uint8_t value;
    __asm__ volatile("inb $0x71, %0" : "=a"(value));
    return value;
}

uint8_t decode_rtc_value(uint8_t value, bool binary_mode) {
    if (binary_mode) return value;
    return static_cast<uint8_t>((value & 0x0F) + ((value >> 4) * 10));
}

void append_two_digits(char* output, uint32_t* length, uint8_t value) {
    output[(*length)++] = static_cast<char>('0' + value / 10);
    output[(*length)++] = static_cast<char>('0' + value % 10);
    output[*length] = '\0';
}

bool read_rtc(uint8_t* second, uint8_t* minute, uint8_t* hour,
              uint8_t* day, uint8_t* month, uint8_t* year) {
    uint32_t retries = 100000;
    while (retries > 0 && (read_cmos(0x0A) & 0x80) != 0) --retries;
    if (retries == 0) return false;
    const uint8_t raw_second = read_cmos(0x00);
    const uint8_t raw_minute = read_cmos(0x02);
    const uint8_t raw_hour = read_cmos(0x04);
    const uint8_t raw_day = read_cmos(0x07);
    const uint8_t raw_month = read_cmos(0x08);
    const uint8_t raw_year = read_cmos(0x09);
    const uint8_t status_b = read_cmos(0x0B);
    const bool binary_mode = (status_b & 0x04) != 0;
    const bool twenty_four_hour = (status_b & 0x02) != 0;
    const bool afternoon = (raw_hour & 0x80) != 0;

    *second = decode_rtc_value(raw_second, binary_mode);
    *minute = decode_rtc_value(raw_minute, binary_mode);
    *hour = decode_rtc_value(static_cast<uint8_t>(raw_hour & 0x7F), binary_mode);
    *day = decode_rtc_value(raw_day, binary_mode);
    *month = decode_rtc_value(raw_month, binary_mode);
    *year = decode_rtc_value(raw_year, binary_mode);
    if (!twenty_four_hour) {
        if (*hour == 12) *hour = 0;
        if (afternoon) *hour = static_cast<uint8_t>(*hour + 12);
    }
    return *month >= 1 && *month <= 12 && *day >= 1 && *day <= 31 &&
           *hour < 24 && *minute < 60 && *second < 60;
}

void add_rtc_report(ConsoleState& console, bool show_date) {
    uint8_t second, minute, hour, day, month, year;
    if (!read_rtc(&second, &minute, &hour, &day, &month, &year)) {
        add_history_line(console, "RTC clock is unavailable.");
        return;
    }

    char line[40] = "";
    uint32_t length = 0;
    if (show_date) {
        length = append_text(line, length, sizeof(line), "Date: 20");
        append_two_digits(line, &length, year);
        line[length++] = '-';
        append_two_digits(line, &length, month);
        line[length++] = '-';
        append_two_digits(line, &length, day);
        line[length] = '\0';
    } else {
        length = append_text(line, length, sizeof(line), "Time: ");
        append_two_digits(line, &length, hour);
        line[length++] = ':';
        append_two_digits(line, &length, minute);
        line[length++] = ':';
        append_two_digits(line, &length, second);
        line[length] = '\0';
    }
    add_history_line(console, line);
}

bool is_default_text_color(Color color) {
    return color.r == kDefaultTextColor.r && color.g == kDefaultTextColor.g &&
           color.b == kDefaultTextColor.b;
}

void set_console_color(ConsoleState& console, const char* name) {
    if (equal_ignore_case(name, "blue")) {
        g_screen_color = kDefaultScreenColor;
        g_text_color = kDefaultTextColor;
        g_prompt_color = kDefaultPromptColor;
    } else if (equal_ignore_case(name, "green")) {
        g_screen_color = {0, 48, 24};
        g_text_color = {220, 255, 220};
        g_prompt_color = {100, 255, 140};
    } else if (equal_ignore_case(name, "amber")) {
        g_screen_color = {48, 24, 0};
        g_text_color = {255, 224, 160};
        g_prompt_color = {255, 190, 70};
    } else {
        add_history_line(console, "Usage: COLOR BLUE | GREEN | AMBER");
        return;
    }
    add_history_line(console, "Console colors updated.", g_prompt_color);
}

[[noreturn]] void reboot_system() {
    uint32_t retries = 100000;
    while (retries > 0 && ([]() {
        uint8_t status;
        __asm__ volatile("inb $0x64, %0" : "=a"(status));
        return status;
    }() & 0x02) != 0) --retries;
    __asm__ volatile("outb %0, $0x64" : : "a"(static_cast<uint8_t>(0xFE)));
    for (;;) __asm__ volatile("cli; hlt" : : : "memory");
}

void add_memory_report(ConsoleState& console, const annyos::BootInfo& boot_info) {
    uint64_t conventional_pages = 0;
    uint64_t descriptor_count = 0;
    if (boot_info.memory_map != nullptr && boot_info.memory_descriptor_size >= sizeof(annyos::MemoryDescriptor)) {
        const auto* bytes = static_cast<const uint8_t*>(boot_info.memory_map);
        descriptor_count = boot_info.memory_map_size / boot_info.memory_descriptor_size;
        for (uint64_t index = 0; index < descriptor_count; ++index) {
            const auto* descriptor = reinterpret_cast<const annyos::MemoryDescriptor*>(
                bytes + index * boot_info.memory_descriptor_size);
            if (descriptor->type == 7) conventional_pages += descriptor->number_of_pages;
        }
    }
    char memory_line[64] = "Conventional memory: ";
    uint32_t length = 21;
    append_decimal(memory_line, &length, conventional_pages / 256);
    const char mb[] = " MB";
    for (uint32_t i = 0; mb[i] != '\0'; ++i) memory_line[length++] = mb[i];
    memory_line[length] = '\0';
    add_history_line(console, memory_line);

    char map_line[48] = "Memory map entries: ";
    length = 20;
    append_decimal(map_line, &length, descriptor_count);
    add_history_line(console, map_line);
}

bool append_fat32_entry(void* context, const annyos::storage::Fat32DirectoryEntry& entry) {
    auto& console = *static_cast<ConsoleState*>(context);
    char line[kLineCapacity];
    uint32_t length = 0;
    while (length + 1 < sizeof(line) && entry.name[length] != '\0') {
        line[length] = entry.name[length];
        ++length;
    }
    while (length < 16) line[length++] = ' ';
    if (entry.is_directory) {
        const char directory[] = "<DIR>";
        for (uint32_t i = 0; directory[i] != '\0'; ++i) line[length++] = directory[i];
    } else {
        append_decimal(line, &length, entry.size);
        const char bytes[] = " bytes";
        for (uint32_t i = 0; bytes[i] != '\0'; ++i) line[length++] = bytes[i];
    }
    line[length] = '\0';
    add_history_line(console, line);
    return true;
}

struct DirectoryLookup {
    const char* name;
    uint32_t cluster;
    bool found;
};

bool find_directory_entry(void* context, const annyos::storage::Fat32DirectoryEntry& entry) {
    auto& lookup = *static_cast<DirectoryLookup*>(context);
    if (entry.is_directory && equal_ignore_case(entry.name, lookup.name)) {
        lookup.cluster = entry.first_cluster;
        lookup.found = true;
    }
    return true;
}

bool find_child_directory(const annyos::storage::Fat32Volume& volume, uint32_t parent_cluster,
                          const char* name, uint32_t* child_cluster) {
    DirectoryLookup lookup{name, 0, false};
    const bool read_ok = annyos::storage::fat32_list_directory(
        volume, parent_cluster, find_directory_entry, &lookup);
    if (!read_ok || !lookup.found) return false;
    *child_cluster = lookup.cluster;
    return true;
}

void reset_working_directory(WorkingDirectory& directory, const annyos::storage::Fat32Volume& volume) {
    directory.depth = 0;
    directory.cluster_stack[0] = volume.root_cluster;
    directory.path[0] = 'C';
    directory.path[1] = ':';
    directory.path[2] = '\\';
    directory.path[3] = '\0';
}

bool change_directory(WorkingDirectory* directory, const annyos::storage::Fat32Volume& volume,
                      const char* target) {
    if (directory == nullptr || target == nullptr) return false;
    while (*target == ' ') ++target;
    if (*target == '\0') return true;

    WorkingDirectory candidate = *directory;
    if (((target[0] >= 'A' && target[0] <= 'Z') ||
         (target[0] >= 'a' && target[0] <= 'z')) && target[1] == ':') {
        if (lowercase(target[0]) != 'c') return false;
        target += 2;
    }
    if (*target == '\\' || *target == '/') reset_working_directory(candidate, volume);

    while (*target != '\0') {
        while (*target == '\\' || *target == '/' || *target == ' ') ++target;
        if (*target == '\0') break;

        char segment[13];
        uint32_t segment_length = 0;
        while (*target != '\0' && *target != '\\' && *target != '/' && *target != ' ') {
            if (segment_length + 1 >= sizeof(segment)) return false;
            segment[segment_length++] = *target++;
        }
        segment[segment_length] = '\0';
        if (equal_ignore_case(segment, ".")) continue;
        if (equal_ignore_case(segment, "..")) {
            if (candidate.depth > 0) {
                --candidate.depth;
                uint32_t path_length = 0;
                while (candidate.path[path_length] != '\0') ++path_length;
                while (path_length > 3 && candidate.path[path_length - 1] != '\\') --path_length;
                if (path_length > 3) --path_length;
                candidate.path[path_length] = '\0';
            }
            continue;
        }
        if (candidate.depth >= kWorkingDirectoryDepth) return false;

        uint32_t child_cluster;
        if (!find_child_directory(volume, candidate.cluster_stack[candidate.depth], segment,
                                  &child_cluster)) return false;
        uint32_t path_length = 0;
        while (candidate.path[path_length] != '\0') ++path_length;
        const uint32_t separator_length = candidate.path[path_length - 1] == '\\' ? 0 : 1;
        if (path_length + separator_length + segment_length >= sizeof(candidate.path)) return false;
        if (separator_length != 0) candidate.path[path_length++] = '\\';
        for (uint32_t i = 0; i < segment_length; ++i) candidate.path[path_length++] = segment[i];
        candidate.path[path_length] = '\0';
        ++candidate.depth;
        candidate.cluster_stack[candidate.depth] = child_cluster;
    }

    *directory = candidate;
    return true;
}

void redraw_history(const annyos::FramebufferInfo& fb, const ConsoleState& console,
                    const WorkingDirectory& working_directory);

bool api_write_line(void* context, const char* text, uint8_t length) {
    auto& state = *static_cast<KernelApiContext*>(context);
    char line[annyos::application::kApMaximumPrintBytes + 1];
    for (uint32_t index = 0; index < length; ++index) line[index] = text[index];
    line[length] = '\0';
    add_history_line(*state.console, line);
    return true;
}

bool api_write_title(void* context, const char* text, uint8_t length) {
    auto& state = *static_cast<KernelApiContext*>(context);
    char line[annyos::application::kApMaximumPrintBytes + 1];
    for (uint32_t index = 0; index < length; ++index) line[index] = text[index];
    line[length] = '\0';
    add_history_line(*state.console, line, kHelpTitleColor, true);
    return true;
}

bool api_write_help_row(void* context, const char* command, uint8_t command_length,
                        const char* description, uint8_t description_length) {
    auto& state = *static_cast<KernelApiContext*>(context);
    char command_text[17];
    char description_text[79];
    for (uint32_t index = 0; index < command_length; ++index) command_text[index] = command[index];
    command_text[command_length] = '\0';
    for (uint32_t index = 0; index < description_length; ++index) {
        description_text[index] = description[index];
    }
    description_text[description_length] = '\0';
    add_help_row(*state.console, command_text, description_text);
    return true;
}

bool api_invoke_service(void* context, annyos::api::Service service) {
    auto& state = *static_cast<KernelApiContext*>(context);
    switch (service) {
        case annyos::api::Service::version:
            add_history_line(*state.console, "AnnyOS Version 0.1");
            return true;
        case annyos::api::Service::date:
            add_rtc_report(*state.console, true);
            return true;
        case annyos::api::Service::time:
            add_rtc_report(*state.console, false);
            return true;
        case annyos::api::Service::memory:
            add_memory_report(*state.console, *state.boot_info);
            return true;
        case annyos::api::Service::reboot:
            add_history_line(*state.console, "Restarting AnnyOS...", g_prompt_color);
            redraw_history(*state.framebuffer, *state.console, *state.working_directory);
            debug_write("AnnyOS: reboot requested\r\n");
            reboot_system();
    }
    return false;
}

annyos::api::AnnyOsApiV1 make_api(KernelApiContext& context) {
    return {sizeof(annyos::api::AnnyOsApiV1), annyos::api::kVersion, &context,
            api_write_line, api_write_title, api_write_help_row, api_invoke_service};
}

bool make_ap_short_name(const char* command, char output[13]) {
    uint32_t length = 0;
    while (command[length] != '\0' && command[length] != ' ') ++length;
    if (length == 0 || command[length] == ' ') return false;

    uint32_t dot = length;
    for (uint32_t index = 0; index < length; ++index) {
        if (command[index] == '.') {
            if (dot != length) return false;
            dot = index;
        }
        if (command[index] == '/' || command[index] == '\\' || command[index] == ':') return false;
    }

    const bool has_extension = dot != length;
    const uint32_t base_length = has_extension ? dot : length;
    const uint32_t extension_length = has_extension ? length - dot - 1 : 2;
    if (base_length == 0 || base_length > 8 || extension_length != 2) return false;
    if (has_extension && (lowercase(command[dot + 1]) != 'a' || lowercase(command[dot + 2]) != 'p')) {
        return false;
    }

    uint32_t output_length = 0;
    for (uint32_t index = 0; index < base_length; ++index) {
        const char character = uppercase(command[index]);
        if (!((character >= 'A' && character <= 'Z') ||
              (character >= '0' && character <= '9') || character == '_' || character == '-')) return false;
        output[output_length++] = character;
    }
    output[output_length++] = '.';
    output[output_length++] = 'A';
    output[output_length++] = 'P';
    output[output_length] = '\0';
    return true;
}

bool run_ap_file(ConsoleState& console, const annyos::storage::Fat32Volume& volume,
                 uint32_t directory_cluster, const char* short_name,
                 const annyos::api::AnnyOsApiV1& api) {
    annyos::storage::Fat32DirectoryEntry entry{};
    if (!annyos::storage::fat32_find_file(volume, directory_cluster, short_name, &entry)) return false;
    if (entry.size > kApBufferCapacity) {
        add_history_line(console, ".AP file exceeds the 64 KiB limit.");
        return true;
    }
    uint32_t bytes_read = 0;
    if (!annyos::storage::fat32_read_file(volume, entry, g_ap_image, kApBufferCapacity, &bytes_read) ||
        bytes_read != entry.size) {
        add_history_line(console, "Could not read the .AP file.");
        return true;
    }

    const annyos::application::ApHost host{&api};
    const annyos::application::ApResult result =
        annyos::application::execute_ap(g_ap_image, bytes_read, host);
    if (result == annyos::application::ApResult::success) return true;
    if (result == annyos::application::ApResult::invalid_checksum) {
        add_history_line(console, ".AP checksum mismatch.");
    } else if (result == annyos::application::ApResult::unsupported_version) {
        add_history_line(console, "Unsupported .AP version.");
    } else {
        add_history_line(console, "Invalid .AP application.");
    }
    return true;
}

bool try_run_ap(ConsoleState& console, const annyos::storage::Fat32Volume& volume,
                bool fat32_mounted, uint32_t directory_cluster,
                const annyos::api::AnnyOsApiV1& api) {
    if (!fat32_mounted) return false;
    char short_name[13];
    if (!make_ap_short_name(console.input, short_name)) return false;
    if (run_ap_file(console, volume, directory_cluster, short_name, api)) return true;

    uint32_t applications_cluster = 0;
    if (find_child_directory(volume, volume.root_cluster, "APPS", &applications_cluster)) {
        if (run_ap_file(console, volume, applications_cluster, short_name, api)) return true;
    }
    return false;
}

void submit_command(ConsoleState& console, const annyos::storage::Fat32Volume& fat32_volume,
                    bool fat32_mounted,
                    WorkingDirectory* working_directory,
                    const annyos::api::AnnyOsApiV1& api) {
    char command_line[kLineCapacity] = "";
    uint32_t length = append_text(command_line, 0, sizeof(command_line), working_directory->path);
    length = append_text(command_line, length, sizeof(command_line), "> ");
    length = append_text(command_line, length, sizeof(command_line), console.input);
    add_history_line(console, command_line);

    if (console.input_length != 0) {
        if (command_equals(console.input, "color")) {
            add_history_line(console, "Usage: COLOR BLUE | GREEN | AMBER");
        } else if (command_starts_with(console.input, "color ")) {
            set_console_color(console, console.input + 6);
        } else if (command_equals(console.input, "dir")) {
            if (!fat32_mounted) {
                add_history_line(console, "FAT32 volume not found.");
            } else {
                char directory_line[kLineCapacity] = "Directory of ";
                const uint32_t directory_length = append_text(directory_line, 13, sizeof(directory_line),
                                                              working_directory->path);
                directory_line[directory_length] = '\0';
                add_history_line(console, directory_line);
                if (!annyos::storage::fat32_list_directory(
                        fat32_volume, working_directory->cluster_stack[working_directory->depth],
                        append_fat32_entry, &console)) {
                    add_history_line(console, "Error reading current directory.");
                }
            }
        } else if (command_equals(console.input, "cd")) {
            char current_line[kLineCapacity] = "Current directory: ";
            const uint32_t current_length = append_text(current_line, 19, sizeof(current_line),
                                                        working_directory->path);
            current_line[current_length] = '\0';
            add_history_line(console, current_line);
        } else if (command_starts_with(console.input, "cd ")) {
            if (!fat32_mounted) {
                add_history_line(console, "FAT32 volume not found.");
            } else if (!change_directory(working_directory, fat32_volume, console.input + 3)) {
                add_history_line(console, "The directory was not found.");
            } else {
                char current_line[kLineCapacity] = "Current directory: ";
                const uint32_t current_length = append_text(current_line, 19, sizeof(current_line),
                                                            working_directory->path);
                current_line[current_length] = '\0';
                add_history_line(console, current_line);
            }
        } else if (command_equals(console.input, "cls")) {
            clear_history(console);
        } else if (command_equals(console.input, "echo")) {
            add_history_line(console, "");
        } else if (command_starts_with(console.input, "echo ")) {
            add_history_line(console, console.input + 5);
        } else if (!try_run_ap(console, fat32_volume, fat32_mounted,
                               working_directory->cluster_stack[working_directory->depth], api)) {
            add_history_line(console, "Bad command or file name.");
        }
    }

    console.input_length = 0;
    console.input[0] = '\0';
}

int32_t visible_rows(const annyos::FramebufferInfo& fb) {
    const int32_t rows = (static_cast<int32_t>(fb.height) - 2 * kMargin) / kLineHeight;
    return rows > 1 ? rows : 1;
}

void draw_prompt(const annyos::FramebufferInfo& fb, const ConsoleState& console,
                 const WorkingDirectory& working_directory, int32_t row) {
    const int32_t y = kMargin + row * kLineHeight;
    fill_rect(fb, 0, y, static_cast<int32_t>(fb.width), kLineHeight, g_screen_color);
    const int32_t path_x = draw_text(fb, kMargin, y, working_directory.path, g_prompt_color);
    const int32_t input_x = draw_text(fb, path_x, y, "> ", g_prompt_color);
    const int32_t cursor_x = draw_text(fb, input_x, y, console.input, kInputColor);
    fill_rect(fb, cursor_x, y + 18, 8, 2, kInputColor);
}

void redraw_history(const annyos::FramebufferInfo& fb, const ConsoleState& console,
                    const WorkingDirectory& working_directory) {
    const int32_t rows = visible_rows(fb);
    fill_rect(fb, 0, 0, static_cast<int32_t>(fb.width), static_cast<int32_t>(fb.height), g_screen_color);
    const int32_t history_rows = rows - 1;
    const uint32_t visible_history = console.history_count < static_cast<uint32_t>(history_rows)
        ? console.history_count : static_cast<uint32_t>(history_rows);
    const uint32_t first_line = console.history_count - visible_history;
    uint32_t visible_index = 0;
    for (uint32_t line = first_line; line < console.history_count; ++line, ++visible_index) {
        const int32_t y = kMargin + static_cast<int32_t>(visible_index) * kLineHeight;
        const uint8_t accent_length = console.history_accent_length[line];
        if (accent_length > 0) {
            char accent[kLineCapacity];
            uint32_t index = 0;
            while (index < accent_length && index + 1 < sizeof(accent)) {
                accent[index] = console.history[line][index];
                ++index;
            }
            accent[index] = '\0';
            const int32_t body_x = draw_text(fb, kMargin, y, accent, g_prompt_color, true);
            if (console.history_columned[line]) {
                draw_text(fb, kMargin + kHelpDescriptionOffset, y,
                          console.history[line] + 16, g_text_color);
            } else {
                draw_text(fb, body_x, y, console.history[line] + accent_length, g_text_color);
            }
        } else {
            const Color color = is_default_text_color(console.history_color[line])
                ? g_text_color : console.history_color[line];
            draw_text(fb, kMargin, y, console.history[line], color,
                      console.history_bold[line]);
        }
    }
    draw_prompt(fb, console, working_directory, static_cast<int32_t>(visible_history));
}

} // namespace

extern "C" [[noreturn]] void kernel_main(const annyos::BootInfo* boot_info) {
    __asm__ volatile("cli" : : : "memory");
    if (boot_info == nullptr || boot_info->framebuffer.address == nullptr ||
        boot_info->framebuffer.width == 0 || boot_info->framebuffer.height == 0) {
        for (;;) __asm__ volatile("hlt");
    }

    annyos::input::initialize_ps2_keyboard();
    annyos::storage::AtaPioDevice ata_device{};
    annyos::storage::BlockDevice block_device{};
    annyos::storage::Fat32Volume fat32_volume{};
    const bool ata_ready = annyos::storage::ata_pio_initialize(0, 0, &ata_device, &block_device);
    const bool fat32_mounted = ata_ready && annyos::storage::fat32_mount(block_device, &fat32_volume);
    debug_write(fat32_mounted ? "AnnyOS: FAT32 volume mounted\r\n"
                              : "AnnyOS: no FAT32 boot disk found\r\n");
    WorkingDirectory& working_directory = g_working_directory;
    reset_working_directory(working_directory, fat32_volume);
    ConsoleState& console = g_console;
    KernelApiContext api_context{&console, boot_info, &boot_info->framebuffer, &working_directory};
    const annyos::api::AnnyOsApiV1 api = make_api(api_context);
    uint32_t applications_cluster = 0;
    const bool has_apps_directory = fat32_mounted &&
        find_child_directory(fat32_volume, fat32_volume.root_cluster, "APPS", &applications_cluster);
    if (!has_apps_directory || !run_ap_file(console, fat32_volume, applications_cluster,
                                              "STARTUP.AP", api)) {
        add_history_line(console, "AnnyOS Command Processor [Version 0.1]");
        add_history_line(console, "Type HELP for commands.");
    }
    redraw_history(boot_info->framebuffer, console, working_directory);
    debug_write("AnnyOS: DOS-style command console ready\r\n");

    for (;;) {
        annyos::input::KeyEvent event{};
        if (!annyos::input::poll_ps2_keyboard(&event)) {
            __asm__ volatile("pause");
            continue;
        }

        if (event.action == annyos::input::KeyAction::enter) {
            submit_command(console, fat32_volume, fat32_mounted, &working_directory, api);
            redraw_history(boot_info->framebuffer, console, working_directory);
        } else if (event.action == annyos::input::KeyAction::backspace) {
            if (console.input_length > 0) console.input[--console.input_length] = '\0';
            redraw_history(boot_info->framebuffer, console, working_directory);
        } else if (event.action == annyos::input::KeyAction::character &&
                   console.input_length + 1 < kInputCapacity && event.character >= 32 && event.character <= 126) {
            const int32_t available_width = static_cast<int32_t>(boot_info->framebuffer.width) - 2 * kMargin;
            const int32_t required_width = measure_text(working_directory.path) + measure_text("> ") +
                                           measure_text(console.input) + get_glyph(event.character).advance;
            if (required_width < available_width) {
                console.input[console.input_length++] = event.character;
                console.input[console.input_length] = '\0';
            }
            redraw_history(boot_info->framebuffer, console, working_directory);
        }
    }
}
