#include "annyos/boot_info.hpp"
#include "annyos/api.hpp"
#include "annyos/ata_pio.hpp"
#include "annyos/ap_format.hpp"
#include "annyos/fat32.hpp"
#include "annyos/font_data.hpp"
#include "annyos/pc_speaker.hpp"
#include "annyos/ps2_keyboard.hpp"
#include "annyos/serial.hpp"
#include "annyos/timer_pit.hpp"

namespace {

struct Color { uint8_t r, g, b; };

constexpr Color kDefaultScreenColor{0, 0, 170};
constexpr Color kDefaultTextColor{220, 220, 220};
constexpr Color kDefaultPromptColor{85, 255, 255};
constexpr Color kDefaultInputColor{255, 255, 255};
constexpr Color kDefaultHelpTitleColor{255, 220, 120};
constexpr int32_t kHelpDescriptionOffset = 128;
constexpr int32_t kDirectoryTypeOffset = 120;
constexpr int32_t kDirectorySizeOffset = 195;
constexpr int32_t kLineHeight = 22;
constexpr int32_t kMargin = 28;
constexpr uint32_t kHistoryCapacity = 32;
constexpr uint32_t kLineCapacity = 100;
constexpr uint32_t kInputCapacity = 88;
constexpr uint32_t kWorkingDirectoryDepth = 12;
constexpr uint32_t kWorkingDirectoryPathCapacity = 80;
constexpr uint32_t kAtaDriveSlots = 4;
constexpr uint32_t kApBufferCapacity = annyos::application::kApMaximumFileBytes;

struct ConsoleState {
    char history[kHistoryCapacity][kLineCapacity];
    Color history_color[kHistoryCapacity];
    uint8_t history_accent_length[kHistoryCapacity];
    bool history_bold[kHistoryCapacity];
    bool history_columned[kHistoryCapacity];
    bool history_tabular[kHistoryCapacity];
    uint32_t history_count;
    char command_history[kHistoryCapacity][kLineCapacity];
    uint32_t command_history_count;
    uint32_t command_history_cursor;
    char input[kInputCapacity];
    uint32_t input_length;
};

struct WorkingDirectory {
    uint32_t cluster_stack[kWorkingDirectoryDepth + 1];
    uint32_t depth;
    uint8_t drive_index;
    char path[kWorkingDirectoryPathCapacity];
};

struct DriveState {
    annyos::storage::AtaPioDevice ata;
    annyos::storage::BlockDevice block;
    annyos::storage::Fat32Volume volume;
    bool mounted;
};

struct KernelApiContext {
    ConsoleState* console;
    const annyos::BootInfo* boot_info;
    const annyos::FramebufferInfo* framebuffer;
    const WorkingDirectory* working_directory;
};

ConsoleState g_console;
WorkingDirectory g_working_directory;
DriveState g_drives[kAtaDriveSlots];
uint8_t g_ap_image[kApBufferCapacity];
Color g_screen_color = kDefaultScreenColor;
Color g_text_color = kDefaultTextColor;
Color g_prompt_color = kDefaultPromptColor;
Color g_input_color = kDefaultInputColor;
Color g_help_title_color = kDefaultHelpTitleColor;
bool g_monochrome_theme = false;
bool g_serial_ready = false;

bool equal_ignore_case(const char* left, const char* right);

void debug_write(const char* text) {
    while (*text != '\0') {
        const uint8_t byte = static_cast<uint8_t>(*text++);
        if (g_serial_ready) annyos::serial::write_byte(byte);
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

uint32_t append_text(char* destination, uint32_t length, uint32_t capacity, const char* text);

void add_directory_row(ConsoleState& console, const char* name, const char* type, const char* size,
                       Color color = kDefaultTextColor, bool bold = false) {
    char line[kLineCapacity];
    uint32_t length = append_text(line, 0, sizeof(line), name);
    if (length + 1 < sizeof(line)) line[length++] = '\t';
    length = append_text(line, length, sizeof(line), type);
    if (length + 1 < sizeof(line)) line[length++] = '\t';
    append_text(line, length, sizeof(line), size);
    add_history_line(console, line, color, bold);
    console.history_tabular[console.history_count - 1] = true;
}

void clear_history(ConsoleState& console) {
    console.history_count = 0;
    console.history[0][0] = '\0';
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

char lowercase(char c);
bool equal_ignore_case(const char* left, const char* right);

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

void add_builtin_help(ConsoleState& console) {
    add_history_line(console, "ANNYOS  |  COMMAND GUIDE", g_help_title_color, true);
    add_history_line(console, "----------------------------------------------------------------");
    add_help_row(console, "COMMAND", "DESCRIPTION");
    add_help_row(console, "DIR", "List the current directory");
    add_help_row(console, "CD [DIR|..|\\]", "Change directory");
    add_help_row(console, "C: THROUGH F:", "Switch to a mounted ATA drive");
    add_help_row(console, "CLS", "Clear the screen");
    add_help_row(console, "ECHO <TEXT>", "Print a message");
    add_help_row(console, "SLEEP", "Wait for 500 milliseconds");
    add_help_row(console, "BEEP", "Play a short PC speaker tone");
    add_help_row(console, "MEM", "Show memory information");
    add_help_row(console, "DATE", "Show the current date");
    add_help_row(console, "TIME", "Show the current time");
    add_help_row(console, "COLOR <THEME>", "Set BLUE, GREEN, AMBER, or MATRIX");
    add_help_row(console, "REBOOT", "Restart the computer");
    add_help_row(console, "NAME[.AP]", "Run an app from the current folder");
    add_help_row(console, "VER", "Show AnnyOS version");
    add_help_row(console, "HELP", "Show this command guide");
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
        g_input_color = kDefaultInputColor;
        g_help_title_color = kDefaultHelpTitleColor;
        g_monochrome_theme = false;
    } else if (equal_ignore_case(name, "green")) {
        g_screen_color = {0, 48, 24};
        g_text_color = {220, 255, 220};
        g_prompt_color = {100, 255, 140};
        g_input_color = {220, 255, 220};
        g_help_title_color = {100, 255, 140};
        g_monochrome_theme = false;
    } else if (equal_ignore_case(name, "amber")) {
        g_screen_color = {48, 24, 0};
        g_text_color = {255, 224, 160};
        g_prompt_color = {255, 190, 70};
        g_input_color = {255, 224, 160};
        g_help_title_color = {255, 190, 70};
        g_monochrome_theme = false;
    } else if (equal_ignore_case(name, "matrix") || equal_ignore_case(name, "black")) {
        g_screen_color = {0, 0, 0};
        g_text_color = {0, 220, 90};
        g_prompt_color = {0, 255, 100};
        g_input_color = {0, 255, 100};
        g_help_title_color = {0, 255, 100};
        g_monochrome_theme = true;
    } else {
        add_history_line(console, "Usage: COLOR BLUE | GREEN | AMBER | MATRIX");
        return;
    }
    add_history_line(console, g_monochrome_theme
        ? "MATRIX theme active: black background, green text."
        : "Console colors updated.", g_prompt_color);
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

struct DirectoryListing {
    ConsoleState* console;
    uint32_t file_count;
    uint32_t directory_count;
    uint64_t total_bytes;
};

bool append_fat32_entry(void* context, const annyos::storage::Fat32DirectoryEntry& entry) {
    auto& listing = *static_cast<DirectoryListing*>(context);
    ConsoleState& console = *listing.console;
    if (entry.is_directory) {
        add_directory_row(console, entry.name, "<DIR>", "--", g_text_color);
        ++listing.directory_count;
    } else {
        char size[24] = "";
        uint32_t size_length = 0;
        append_decimal(size, &size_length, entry.size);
        append_text(size, size_length, sizeof(size), " bytes");
        add_directory_row(console, entry.name, "FILE", size);
        ++listing.file_count;
        listing.total_bytes += entry.size;
    }
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

void reset_working_directory(WorkingDirectory& directory, const annyos::storage::Fat32Volume& volume,
                             char drive_letter, uint8_t drive_index) {
    directory.depth = 0;
    directory.drive_index = drive_index;
    directory.cluster_stack[0] = volume.root_cluster;
    directory.path[0] = drive_letter;
    directory.path[1] = ':';
    directory.path[2] = '\\';
    directory.path[3] = '\0';
}

uint32_t bounded_string_length(const char* text, uint32_t capacity) {
    uint32_t length = 0;
    while (length < capacity && text[length] != '\0') ++length;
    return length;
}

bool valid_working_directory(const WorkingDirectory& directory) {
    if (directory.depth > kWorkingDirectoryDepth) return false;
    const uint32_t length = bounded_string_length(directory.path, sizeof(directory.path));
    return length < sizeof(directory.path) && length >= 3 &&
           directory.path[0] >= 'C' && directory.path[0] <= 'F' &&
           directory.path[1] == ':' && directory.path[2] == '\\';
}

bool change_directory(WorkingDirectory* directory, const annyos::storage::Fat32Volume& volume,
                      const char* target) {
    if (directory == nullptr || target == nullptr || !valid_working_directory(*directory)) return false;
    while (*target == ' ') ++target;
    if (*target == '\0') return true;

    WorkingDirectory candidate = *directory;
    if (((target[0] >= 'A' && target[0] <= 'Z') ||
         (target[0] >= 'a' && target[0] <= 'z')) && target[1] == ':') {
        if (uppercase(target[0]) != directory->path[0]) return false;
        target += 2;
    }
    if (*target == '\\' || *target == '/') {
        reset_working_directory(candidate, volume, directory->path[0], directory->drive_index);
    }

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
                const uint32_t path_length = bounded_string_length(candidate.path, sizeof(candidate.path));
                if (path_length >= sizeof(candidate.path)) return false;
                uint32_t parent_length = path_length;
                while (parent_length > 3 && candidate.path[parent_length - 1] != '\\') --parent_length;
                if (parent_length > 3) --parent_length;
                candidate.path[parent_length] = '\0';
            }
            continue;
        }
        if (candidate.depth >= kWorkingDirectoryDepth) return false;

        uint32_t child_cluster;
        if (!find_child_directory(volume, candidate.cluster_stack[candidate.depth], segment,
                                  &child_cluster)) return false;
        const uint32_t path_length = bounded_string_length(candidate.path, sizeof(candidate.path));
        if (path_length >= sizeof(candidate.path) || path_length < 3) return false;
        const uint32_t separator_length = candidate.path[path_length - 1] == '\\' ? 0 : 1;
        const uint32_t addition_length = separator_length + segment_length;
        if (addition_length >= sizeof(candidate.path) - path_length) return false;
        uint32_t write_offset = path_length;
        if (separator_length != 0) candidate.path[write_offset++] = '\\';
        for (uint32_t i = 0; i < segment_length; ++i) candidate.path[write_offset++] = segment[i];
        candidate.path[write_offset] = '\0';
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
    add_history_line(*state.console, line, g_help_title_color, true);
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
            add_history_line(*state.console, "AnnyOS Version 0.2");
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
        case annyos::api::Service::sleep:
            if (!annyos::timer::sleep_ms(500)) {
                add_history_line(*state.console, "PIT timer is unavailable.");
            } else {
                add_history_line(*state.console, "Sleep complete.", g_prompt_color);
            }
            return true;
        case annyos::api::Service::beep:
            if (!annyos::speaker::tone(880, 140)) {
                add_history_line(*state.console, "PC speaker is unavailable.");
            } else {
                add_history_line(*state.console, "Beep.", g_prompt_color);
            }
            return true;
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
    if (length == 0) return false;

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

bool parse_ap_arguments(const char* source, annyos::application::ApArguments* arguments) {
    if (source == nullptr || arguments == nullptr) return false;
    arguments->count = 0;
    while (*source == ' ') ++source;
    while (*source != '\0') {
        if (arguments->count >= annyos::application::kApMaximumArguments) return false;
        const uint8_t index = arguments->count++;
        uint32_t length = 0;
        char quote = '\0';
        bool token_started = false;
        while (*source != '\0') {
            const char character = *source++;
            if (quote != '\0') {
                if (character == quote) {
                    quote = '\0';
                    token_started = true;
                    continue;
                }
            } else if (character == '"' || character == '\'') {
                quote = character;
                token_started = true;
                continue;
            } else if (character == ' ') {
                break;
            }
            if (length >= annyos::application::kApMaximumArgumentBytes) return false;
            arguments->values[index][length++] = character;
            token_started = true;
        }
        if (quote != '\0') return false;
        arguments->values[index][length] = '\0';
        if (!token_started) return false;
        while (*source == ' ') ++source;
    }
    return true;
}

bool run_ap_file(ConsoleState& console, const annyos::storage::Fat32Volume& volume,
                 uint32_t directory_cluster, const char* short_name,
                 const annyos::api::AnnyOsApiV1& api, const char* raw_arguments) {
    annyos::storage::Fat32DirectoryEntry entry{};
    if (!annyos::storage::fat32_find_file(volume, directory_cluster, short_name, &entry)) return false;
    annyos::application::ApArguments arguments{};
    if (!parse_ap_arguments(raw_arguments, &arguments)) {
        add_history_line(console, "Invalid app arguments. Use up to 8 quoted or unquoted values.");
        return true;
    }
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
        annyos::application::execute_ap(g_ap_image, bytes_read, host, &arguments);
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
    const char* argument_start = console.input;
    while (*argument_start != '\0' && *argument_start != ' ') ++argument_start;
    while (*argument_start == ' ') ++argument_start;
    if (run_ap_file(console, volume, directory_cluster, short_name, api, argument_start)) return true;

    uint32_t applications_cluster = 0;
    if (find_child_directory(volume, volume.root_cluster, "APPS", &applications_cluster)) {
        if (run_ap_file(console, volume, applications_cluster, short_name, api, argument_start)) return true;
    }
    return false;
}

bool switch_drive(ConsoleState& console, DriveState* drives, WorkingDirectory* working_directory) {
    const char drive = uppercase(console.input[0]);
    if (console.input[1] != ':' || drive < 'C' || drive > 'F') return false;
    const uint8_t index = static_cast<uint8_t>(drive - 'C');
    if (!drives[index].mounted) {
        char message[] = "Drive X: is not available.";
        message[6] = drive;
        add_history_line(console, message);
        return true;
    }

    reset_working_directory(*working_directory, drives[index].volume, drive, index);
    const char* target = console.input + 2;
    if (*target != '\0' && !change_directory(working_directory, drives[index].volume, target)) {
        add_history_line(console, "The directory was not found on that drive.");
        reset_working_directory(*working_directory, drives[index].volume, drive, index);
        return true;
    }
    char message[kLineCapacity] = "Current directory: ";
    const uint32_t length = append_text(message, 19, sizeof(message), working_directory->path);
    message[length] = '\0';
    add_history_line(console, message);
    return true;
}

void submit_command(ConsoleState& console, DriveState* drives,
                    WorkingDirectory* working_directory,
                    const annyos::api::AnnyOsApiV1& api) {
    DriveState& active_drive = drives[working_directory->drive_index];
    const bool fat32_mounted = active_drive.mounted;
    const annyos::storage::Fat32Volume& fat32_volume = active_drive.volume;
    char command_line[kLineCapacity] = "";
    uint32_t length = append_text(command_line, 0, sizeof(command_line), working_directory->path);
    length = append_text(command_line, length, sizeof(command_line), "> ");
    length = append_text(command_line, length, sizeof(command_line), console.input);
    add_history_line(console, command_line);

    if (console.input_length != 0) {
        remember_command(console);
        if (switch_drive(console, drives, working_directory)) {
            // Drive changes are handled before commands so C:, D:, and paths
            // such as E:\APPS switch the active FAT32 volume.
        } else if (command_equals(console.input, "color")) {
            add_history_line(console, "Usage: COLOR BLUE | GREEN | AMBER | MATRIX");
        } else if (command_starts_with(console.input, "color ")) {
            set_console_color(console, console.input + 6);
        } else if (command_equals(console.input, "dir")) {
            if (!fat32_mounted) {
                add_history_line(console, "FAT32 volume not found.");
            } else {
                char directory_line[kLineCapacity] = "DIRECTORY  ";
                const uint32_t directory_length = append_text(directory_line, 11, sizeof(directory_line),
                                                              working_directory->path);
                directory_line[directory_length] = '\0';
                add_history_line(console, directory_line, g_help_title_color, true);
                add_history_line(console, "------------------------------------------------");
                add_directory_row(console, "NAME", "TYPE", "SIZE", g_prompt_color, true);
                DirectoryListing listing{&console, 0, 0, 0};
                if (!annyos::storage::fat32_list_directory(
                        fat32_volume, working_directory->cluster_stack[working_directory->depth],
                        append_fat32_entry, &listing)) {
                    add_history_line(console, "Error reading current directory.");
                } else {
                    char summary[kLineCapacity] = "";
                    uint32_t summary_length = 0;
                    append_decimal(summary, &summary_length, listing.file_count);
                    summary_length = append_text(summary, summary_length, sizeof(summary),
                        listing.file_count == 1 ? " file    " : " files    ");
                    append_decimal(summary, &summary_length, listing.directory_count);
                    summary_length = append_text(summary, summary_length, sizeof(summary),
                        listing.directory_count == 1 ? " folder    " : " folders    ");
                    append_decimal(summary, &summary_length, listing.total_bytes);
                    append_text(summary, summary_length, sizeof(summary), " bytes total");
                    add_history_line(console, summary, g_prompt_color);
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
            if (command_equals(console.input, "help") || command_equals(console.input, "help.ap")) {
                add_builtin_help(console);
            } else {
                add_history_line(console, "Bad command or file name.");
            }
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
    const int32_t cursor_x = draw_text(fb, input_x, y, console.input, g_input_color);
    fill_rect(fb, cursor_x, y + 18, 8, 2, g_input_color);
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
        if (console.history_tabular[line]) {
            char fields[3][24]{};
            uint32_t field = 0;
            uint32_t field_length = 0;
            for (const char* cursor = console.history[line]; *cursor != '\0'; ++cursor) {
                if (*cursor == '\t') {
                    if (field < 2) ++field;
                    field_length = 0;
                } else if (field < 3 && field_length + 1 < sizeof(fields[field])) {
                    fields[field][field_length++] = *cursor;
                }
            }
            draw_text(fb, kMargin, y, fields[0], g_text_color, console.history_bold[line]);
            draw_text(fb, kMargin + kDirectoryTypeOffset, y, fields[1], g_prompt_color,
                      console.history_bold[line]);
            draw_text(fb, kMargin + kDirectorySizeOffset, y, fields[2], g_text_color,
                      console.history_bold[line]);
        } else if (accent_length > 0) {
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
            const Color color = g_monochrome_theme ? g_text_color :
                (is_default_text_color(console.history_color[line])
                    ? g_text_color : console.history_color[line]);
            draw_text(fb, kMargin, y, console.history[line], color,
                      console.history_bold[line]);
        }
    }
    draw_prompt(fb, console, working_directory, static_cast<int32_t>(visible_history));
}

void redraw_prompt(const annyos::FramebufferInfo& fb, const ConsoleState& console,
                   const WorkingDirectory& working_directory) {
    const int32_t history_rows = visible_rows(fb) - 1;
    const uint32_t visible_history = console.history_count < static_cast<uint32_t>(history_rows)
        ? console.history_count : static_cast<uint32_t>(history_rows);
    draw_prompt(fb, console, working_directory, static_cast<int32_t>(visible_history));
}

} // namespace

extern "C" [[noreturn]] void kernel_main(const annyos::BootInfo* boot_info) {
    __asm__ volatile("cli" : : : "memory");
    g_serial_ready = annyos::serial::initialize(0x3f8);
    const bool timer_ready = annyos::timer::initialize(1000);
    debug_write(g_serial_ready ? "AnnyOS: COM1 serial logging enabled\r\n"
                               : "AnnyOS: COM1 serial port unavailable\r\n");
    debug_write(timer_ready ? "AnnyOS: PIT timer initialized at 1000 Hz\r\n"
                            : "AnnyOS: PIT timer initialization failed\r\n");
    if (boot_info == nullptr || boot_info->framebuffer.address == nullptr ||
        boot_info->framebuffer.width == 0 || boot_info->framebuffer.height == 0) {
        for (;;) __asm__ volatile("hlt");
    }

    annyos::input::initialize_ps2_keyboard();
    for (uint8_t index = 0; index < kAtaDriveSlots; ++index) {
        DriveState& drive = g_drives[index];
        const uint8_t channel = index / 2;
        const uint8_t device = index % 2;
        const bool ata_ready = annyos::storage::ata_pio_initialize(
            channel, device, &drive.ata, &drive.block);
        drive.mounted = ata_ready && annyos::storage::fat32_mount(drive.block, &drive.volume);
        if (drive.mounted) {
            debug_write(index == 0 ? "AnnyOS: FAT32 drive C: mounted\r\n"
                                   : index == 1 ? "AnnyOS: FAT32 drive D: mounted\r\n"
                                   : index == 2 ? "AnnyOS: FAT32 drive E: mounted\r\n"
                                                : "AnnyOS: FAT32 drive F: mounted\r\n");
        }
    }
    if (!g_drives[0].mounted) debug_write("AnnyOS: no FAT32 boot disk found\r\n");
    WorkingDirectory& working_directory = g_working_directory;
    reset_working_directory(working_directory, g_drives[0].volume, 'C', 0);
    ConsoleState& console = g_console;
    KernelApiContext api_context{&console, boot_info, &boot_info->framebuffer, &working_directory};
    const annyos::api::AnnyOsApiV1 api = make_api(api_context);
    uint32_t applications_cluster = 0;
    const bool has_apps_directory = g_drives[0].mounted &&
        find_child_directory(g_drives[0].volume, g_drives[0].volume.root_cluster,
                             "APPS", &applications_cluster);
    if (!has_apps_directory || !run_ap_file(console, g_drives[0].volume, applications_cluster,
                                              "STARTUP.AP", api, "")) {
        add_history_line(console, "AnnyOS Command Processor [Version 0.2]");
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
            submit_command(console, g_drives, &working_directory, api);
            redraw_history(boot_info->framebuffer, console, working_directory);
        } else if (event.action == annyos::input::KeyAction::up ||
                   event.action == annyos::input::KeyAction::down) {
            recall_command(console, event.action == annyos::input::KeyAction::up);
            redraw_prompt(boot_info->framebuffer, console, working_directory);
        } else if (event.action == annyos::input::KeyAction::backspace) {
            console.command_history_cursor = console.command_history_count;
            if (console.input_length > 0) console.input[--console.input_length] = '\0';
            redraw_prompt(boot_info->framebuffer, console, working_directory);
        } else if (event.action == annyos::input::KeyAction::character &&
                   console.input_length + 1 < kInputCapacity && event.character >= 32 && event.character <= 126) {
            console.command_history_cursor = console.command_history_count;
            const int32_t available_width = static_cast<int32_t>(boot_info->framebuffer.width) - 2 * kMargin;
            const int32_t required_width = measure_text(working_directory.path) + measure_text("> ") +
                                           measure_text(console.input) + get_glyph(event.character).advance;
            if (required_width < available_width) {
                console.input[console.input_length++] = event.character;
                console.input[console.input_length] = '\0';
            }
            redraw_prompt(boot_info->framebuffer, console, working_directory);
        }
    }
}
