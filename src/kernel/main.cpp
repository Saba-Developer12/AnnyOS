#include "annyos/boot_info.hpp"
#include "annyos/api.hpp"
#include "annyos/ata_pio.hpp"
#include "annyos/ap_format.hpp"
#include "annyos/config_save.hpp"
#include "annyos/driver_registry.hpp"
#include "annyos/driver_services.hpp"
#include "annyos/apfs.hpp"
#include "annyos/fat32.hpp"
#include "annyos/font_data.hpp"
#include "annyos/ntfs.hpp"
#include "annyos/pc_speaker.hpp"
#include "annyos/ps2_keyboard.hpp"
#include "annyos/serial.hpp"
#include "annyos/timer_pit.hpp"
#include "annyos/usb_hid.hpp"

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
constexpr uint32_t kFileBufferCapacity = 4096;
constexpr uint32_t kApBufferCapacity = annyos::application::kApMaximumFileBytes;
constexpr uint32_t kApiMemoryBytes = 8192;
constexpr uint32_t kApiMemoryBlocks = 16;
constexpr uint32_t kEnvironmentSlots = 16;

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
    bool editor_active;
    char editor_filename[13];
    uint8_t editor_buffer[kFileBufferCapacity];
    uint32_t editor_length;
    uint32_t editor_cursor;
    char editor_status[kLineCapacity];
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
    bool ata_ready;
    bool mounted;
};

struct ApiMemoryBlock {
    uint32_t offset;
    uint32_t size;
    bool used;
};

struct EnvironmentEntry {
    char key[16];
    char value[64];
    bool used;
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
annyos::config::ConfigSave g_config_save{};
uint8_t g_ap_image[kApBufferCapacity];
Color g_screen_color = kDefaultScreenColor;
Color g_text_color = kDefaultTextColor;
Color g_prompt_color = kDefaultPromptColor;
Color g_input_color = kDefaultInputColor;
Color g_help_title_color = kDefaultHelpTitleColor;
bool g_monochrome_theme = false;
bool g_serial_ready = false;
annyos::drivers::DriverRegistry g_driver_registry{};
uint8_t g_api_memory[kApiMemoryBytes]{};
ApiMemoryBlock g_api_memory_blocks[kApiMemoryBlocks]{};
EnvironmentEntry g_environment[kEnvironmentSlots]{};
uint8_t g_audio_volume = 100;

bool driver_get_fat32_volume(void* context, uint8_t drive_index,
                             const annyos::storage::Fat32Volume** volume);
bool driver_get_ata_device(void* context, uint8_t drive_index,
                           const annyos::storage::BlockDevice** device);
const annyos::drivers::service::SerialOutputV1 g_serial_service{
    annyos::serial::write_byte, annyos::serial::write};
const annyos::drivers::service::MonotonicTimerV1 g_timer_service{
    annyos::timer::ticks, annyos::timer::sleep_ms};
const annyos::drivers::service::KeyboardInputV1 g_keyboard_service{
    annyos::input::poll_ps2_keyboard};
annyos::drivers::service::Fat32DriveSetV1 g_fat32_service{
    g_drives, driver_get_fat32_volume};
annyos::drivers::service::AtaDriveSetV1 g_ata_service{
    g_drives, driver_get_ata_device};

bool equal_ignore_case(const char* left, const char* right);
uint32_t append_text(char* destination, uint32_t length, uint32_t capacity, const char* text);
void append_decimal(char* output, uint32_t* length, uint64_t value);

void debug_write(const char* text) {
    while (*text != '\0') {
        const uint8_t byte = static_cast<uint8_t>(*text++);
        if (g_serial_ready) annyos::serial::write_byte(byte);
        __asm__ volatile("outb %0, $0xe9" : : "a"(byte));
    }
}

bool driver_get_fat32_volume(void* context, uint8_t drive_index,
                             const annyos::storage::Fat32Volume** volume) {
    if (context == nullptr || volume == nullptr || drive_index >= kAtaDriveSlots) return false;
    const auto* drives = static_cast<const DriveState*>(context);
    if (!drives[drive_index].mounted) return false;
    *volume = &drives[drive_index].volume;
    return true;
}

bool driver_get_ata_device(void* context, uint8_t drive_index,
                           const annyos::storage::BlockDevice** device) {
    if (context == nullptr || device == nullptr || drive_index >= kAtaDriveSlots) return false;
    const auto* drives = static_cast<const DriveState*>(context);
    if (!drives[drive_index].ata_ready) return false;
    *device = &drives[drive_index].block;
    return true;
}

bool initialize_serial_driver(void*, annyos::drivers::DriverRegistry& registry) {
    g_serial_ready = annyos::serial::initialize(0x3f8);
    if (!g_serial_ready || !registry.publish(annyos::drivers::ServiceId::serial_output,
                                              1, &g_serial_service)) return false;
    debug_write("AnnyOS: COM1 serial driver started\r\n");
    return true;
}

bool initialize_timer_driver(void*, annyos::drivers::DriverRegistry& registry) {
    if (!annyos::timer::initialize(1000) ||
        !registry.publish(annyos::drivers::ServiceId::monotonic_timer, 1, &g_timer_service)) {
        return false;
    }
    debug_write("AnnyOS: PIT timer driver started at 1000 Hz\r\n");
    return true;
}

bool initialize_keyboard_driver(void*, annyos::drivers::DriverRegistry& registry) {
    annyos::input::initialize_ps2_keyboard();
    return registry.publish(annyos::drivers::ServiceId::keyboard_input, 1, &g_keyboard_service);
}

bool initialize_ata_driver(void*, annyos::drivers::DriverRegistry& registry) {
    for (uint8_t index = 0; index < kAtaDriveSlots; ++index) {
        DriveState& drive = g_drives[index];
        const uint8_t channel = index / 2;
        const uint8_t device = index % 2;
        drive.ata_ready = annyos::storage::ata_pio_initialize(
            channel, device, &drive.ata, &drive.block);
        if (drive.ata_ready) {
            debug_write(index == 0 ? "AnnyOS: ATA drive C: detected\r\n"
                                   : index == 1 ? "AnnyOS: ATA drive D: detected\r\n"
                                   : index == 2 ? "AnnyOS: ATA drive E: detected\r\n"
                                                : "AnnyOS: ATA drive F: detected\r\n");
        }
    }
    return registry.publish(annyos::drivers::ServiceId::ata_devices, 1, &g_ata_service);
}

bool initialize_fat32_driver(void*, annyos::drivers::DriverRegistry& registry) {
    const auto* ata_record = registry.query(annyos::drivers::ServiceId::ata_devices, 1);
    if (ata_record == nullptr) return false;
    const auto* ata = static_cast<const annyos::drivers::service::AtaDriveSetV1*>(
        ata_record->interface_pointer);
    for (uint8_t index = 0; index < kAtaDriveSlots; ++index) {
        const annyos::storage::BlockDevice* device = nullptr;
        if (!ata->get_device(ata->context, index, &device)) continue;
        DriveState& drive = g_drives[index];
        drive.mounted = annyos::storage::fat32_mount(*device, &drive.volume);
        if (drive.mounted) {
            debug_write(index == 0 ? "AnnyOS: FAT32 drive C: mounted\r\n"
                                   : index == 1 ? "AnnyOS: FAT32 drive D: mounted\r\n"
                                   : index == 2 ? "AnnyOS: FAT32 drive E: mounted\r\n"
                                                : "AnnyOS: FAT32 drive F: mounted\r\n");
            continue;
        }
        annyos::storage::NtfsVolumeInfo ntfs_info{};
        if (annyos::storage::ntfs_probe(*device, &ntfs_info)) {
            debug_write(index == 0 ? "AnnyOS: NTFS drive C: detected; mount support not implemented\r\n"
                                   : index == 1 ? "AnnyOS: NTFS drive D: detected; mount support not implemented\r\n"
                                   : index == 2 ? "AnnyOS: NTFS drive E: detected; mount support not implemented\r\n"
                                                : "AnnyOS: NTFS drive F: detected; mount support not implemented\r\n");
            continue;
        }
        annyos::storage::ApfsContainerInfo apfs_info{};
        if (annyos::storage::apfs_probe(*device, &apfs_info)) {
            debug_write(index == 0 ? "AnnyOS: APFS drive C: detected; mount support not implemented\r\n"
                                   : index == 1 ? "AnnyOS: APFS drive D: detected; mount support not implemented\r\n"
                                   : index == 2 ? "AnnyOS: APFS drive E: detected; mount support not implemented\r\n"
                                                : "AnnyOS: APFS drive F: detected; mount support not implemented\r\n");
        }
    }
    if (!g_drives[0].mounted) debug_write("AnnyOS: no FAT32 boot disk found\r\n");
    return registry.publish(annyos::drivers::ServiceId::fat32_volumes, 1, &g_fat32_service);
}

void initialize_kernel_drivers() {
    using namespace annyos::drivers;
    static const ServiceRequirement fat32_requirements[] = {
        {ServiceId::ata_devices, 1},
    };
    static const DriverDescriptor descriptors[] = {
        {DriverId::serial_com1, "serial.com1", 1, nullptr, 0, initialize_serial_driver},
        {DriverId::timer_pit, "timer.pit", 1, nullptr, 0, initialize_timer_driver},
        {DriverId::ps2_keyboard, "input.ps2kbd", 1, nullptr, 0, initialize_keyboard_driver},
        {DriverId::ata_pio, "storage.ata-pio", 1, nullptr, 0, initialize_ata_driver},
        {DriverId::fat32, "filesystem.fat32", 1, fat32_requirements, 1, initialize_fat32_driver},
    };

    g_driver_registry.reset();
    for (const DriverDescriptor& descriptor : descriptors) {
        if (!g_driver_registry.register_driver(descriptor)) {
            debug_write("AnnyOS: driver registry rejected a built-in driver\r\n");
        }
    }
    const InitializationSummary summary = g_driver_registry.initialize_all(nullptr);
    char message[64] = "AnnyOS: driver startup complete: ";
    uint32_t length = 33;
    append_decimal(message, &length, summary.started);
    length = append_text(message, length, sizeof(message), " started, ");
    append_decimal(message, &length, summary.failed);
    length = append_text(message, length, sizeof(message), " failed, ");
    append_decimal(message, &length, summary.blocked);
    append_text(message, length, sizeof(message), " blocked\r\n");
    debug_write(message);
}

void debug_write_hex(uint64_t value) {
    static constexpr char digits[] = "0123456789abcdef";
    char text[16];
    for (uint32_t index = 0; index < 16; ++index) {
        const uint32_t shift = (15 - index) * 4;
        text[index] = digits[(value >> shift) & 0x0f];
    }
    for (char digit : text) {
        if (g_serial_ready) annyos::serial::write_byte(static_cast<uint8_t>(digit));
        __asm__ volatile("outb %0, $0xe9" : : "a"(static_cast<uint8_t>(digit)));
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
    add_help_row(console, "DIR [PATH]", "List current directory or PATH");
    add_help_row(console, "CD [DIR|..|\\]", "Change directory");
    add_help_row(console, "C: THROUGH F:", "Switch to a mounted ATA drive");
    add_help_row(console, "CREATE <FILE>", "Create empty file or use CREATE <FILE> TEXT");
    add_help_row(console, "EDIT <FILE>", "Arrows edit text; F2 saves, ESC cancels");
    add_help_row(console, "READ <FILE>", "Print a file's text contents");
    add_help_row(console, "DELETE <FILE>", "Delete a file and release its clusters");
    add_help_row(console, "CLS", "Clear the screen");
    add_help_row(console, "ECHO <TEXT>", "Print a message");
    add_help_row(console, "SLEEP", "Wait for 500 milliseconds");
    add_help_row(console, "BEEP", "Play a short PC speaker tone");
    add_help_row(console, "MEM", "Show memory information");
    add_help_row(console, "DATE", "Show the current date");
    add_help_row(console, "TIME", "Show the current time");
    add_help_row(console, "COLOR <THEME>", "Set BLUE, GREEN, AMBER, or MATRIX");
    add_help_row(console, "CONFIGSAVE", "View or change saved settings");
    add_help_row(console, "CFGDEMO", "Show ConfigSave API state");
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

bool apply_console_theme(const char* name) {
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
        return false;
    }
    return true;
}

const char* canonical_console_theme(const char* name) {
    if (equal_ignore_case(name, "blue")) return "BLUE";
    if (equal_ignore_case(name, "green")) return "GREEN";
    if (equal_ignore_case(name, "amber")) return "AMBER";
    if (equal_ignore_case(name, "matrix") || equal_ignore_case(name, "black")) return "MATRIX";
    return nullptr;
}

void set_console_color(ConsoleState& console, const char* name) {
    const char* theme = canonical_console_theme(name);
    if (theme == nullptr || !apply_console_theme(theme)) {
        add_history_line(console, "Usage: COLOR BLUE | GREEN | AMBER | MATRIX");
        return;
    }
    if (!annyos::config::config_save_set_theme(&g_config_save, theme)) {
        add_history_line(console, "Theme applied, but settings could not be saved.", g_prompt_color);
        return;
    }
    add_history_line(console, g_monochrome_theme
        ? "MATRIX theme active and saved."
        : "Console theme applied and saved.", g_prompt_color);
}

[[noreturn]] void reboot_system(const annyos::BootInfo& boot_info) {
    __asm__ volatile("cli" : : : "memory");
    // ResetSystem is a UEFI Runtime Service and remains valid after
    // ExitBootServices. A conforming implementation does not return.
    if (boot_info.reset_system != nullptr) {
        boot_info.reset_system(0, 0, 0, nullptr); // EfiResetCold, EFI_SUCCESS.
        boot_info.reset_system(1, 0, 0, nullptr); // EfiResetWarm fallback.
    }

    // Common PC chipset reset-control register. Do not spin waiting on a
    // device that may be absent or wedged.
    __asm__ volatile("outb %0, %w1" : : "a"(static_cast<uint8_t>(0x02)), "Nd"(0x0cf9));
    __asm__ volatile("outb %0, %w1" : : "a"(static_cast<uint8_t>(0x06)), "Nd"(0x0cf9));

    // Legacy i8042 reset as a final hardware fallback.
    uint32_t retries = 100000;
    while (retries-- != 0) {
        uint8_t status;
        __asm__ volatile("inb %1, %0" : "=a"(status) : "Nd"(0x64));
        if ((status & 0x02u) == 0) {
            __asm__ volatile("outb %0, %1" : : "a"(static_cast<uint8_t>(0xfe)), "Nd"(0x64));
            break;
        }
        __asm__ volatile("pause");
    }

    // If the platform has none of the reset mechanisms above, deliberately
    // triple-fault rather than leaving a nonresponsive shell running.
    struct __attribute__((packed)) EmptyIdt { uint16_t limit; uint64_t base; } empty_idt{0, 0};
    __asm__ volatile("lidt %0; int3" : : "m"(empty_idt) : "memory");
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
void list_directory(ConsoleState& console, const annyos::storage::Fat32Volume& volume,
                    const WorkingDirectory& current_directory, const char* requested_path);
void begin_file_edit(ConsoleState& console, const annyos::storage::Fat32Volume& volume,
                     uint32_t directory_cluster, const char* filename);
bool is_text_file(const uint8_t* data, uint32_t length);
void display_file_contents(ConsoleState& console, const uint8_t* data, uint32_t length);

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
            add_history_line(*state.console, "AnnyOS Version 0.4.4");
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
            reboot_system(*state.boot_info);
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
        case annyos::api::Service::config_list: {
            add_history_line(*state.console, "ANNYOS | PERSISTENT SETTINGS", g_help_title_color, true);
            char line[40] = "THEME=";
            append_text(line, 6, sizeof(line),
                        annyos::config::config_save_active_theme(&g_config_save));
            add_history_line(*state.console, line, g_prompt_color);
            add_history_line(*state.console, "File: C:\\ANNYOS.CFG");
            return true;
        }
        case annyos::api::Service::config_get_theme: {
            char line[40] = "THEME=";
            append_text(line, 6, sizeof(line),
                        annyos::config::config_save_active_theme(&g_config_save));
            add_history_line(*state.console, line, g_prompt_color);
            return true;
        }
        case annyos::api::Service::config_set_theme_blue:
            set_console_color(*state.console, "BLUE");
            return true;
        case annyos::api::Service::config_set_theme_green:
            set_console_color(*state.console, "GREEN");
            return true;
        case annyos::api::Service::config_set_theme_amber:
            set_console_color(*state.console, "AMBER");
            return true;
        case annyos::api::Service::config_set_theme_matrix:
            set_console_color(*state.console, "MATRIX");
            return true;
        default:
            return false;
    }
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

const char* api_argument(const annyos::api::ServiceArguments* arguments, uint32_t index) {
    return arguments != nullptr && index < arguments->count ? arguments->values[index] : "";
}

void add_api_decimal(ConsoleState& console, const char* label, uint64_t value) {
    char line[80] = "";
    uint32_t length = append_text(line, 0, sizeof(line), label);
    append_decimal(line, &length, value);
    add_history_line(console, line, g_prompt_color);
}

bool api_environment_set(ConsoleState& console,
                         const annyos::api::ServiceArguments* arguments) {
    const char* key = api_argument(arguments, 0);
    const char* value = api_argument(arguments, 1);
    const uint32_t key_length = bounded_string_length(key, sizeof(g_environment[0].key));
    const uint32_t value_length = bounded_string_length(value, sizeof(g_environment[0].value));
    if (key_length == 0 || key_length >= sizeof(g_environment[0].key) ||
        value_length >= sizeof(g_environment[0].value)) {
        add_history_line(console, "SYS_SET_ENV requires KEY and VALUE (15/63 byte limits).");
        return true;
    }
    for (uint32_t index = 0; index < key_length; ++index) {
        const char c = key[index];
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
              (c >= '0' && c <= '9') || c == '_')) {
            add_history_line(console, "Environment keys use letters, digits, and underscore.");
            return true;
        }
    }
    uint32_t slot = kEnvironmentSlots;
    for (uint32_t index = 0; index < kEnvironmentSlots; ++index) {
        if (g_environment[index].used && equal_ignore_case(g_environment[index].key, key)) {
            slot = index;
            break;
        }
        if (!g_environment[index].used && slot == kEnvironmentSlots) slot = index;
    }
    if (slot == kEnvironmentSlots) {
        add_history_line(console, "Environment table is full.");
        return true;
    }
    append_text(g_environment[slot].key, 0, sizeof(g_environment[slot].key), key);
    append_text(g_environment[slot].value, 0, sizeof(g_environment[slot].value), value);
    g_environment[slot].used = true;
    add_history_line(console, "Environment value stored for this session.", g_prompt_color);
    return true;
}

bool api_memory_allocate(ConsoleState& console,
                         const annyos::api::ServiceArguments* arguments) {
    uint32_t requested = 0;
    if (!parse_decimal(api_argument(arguments, 0), &requested) || requested == 0 ||
        requested > kApiMemoryBytes) {
        add_history_line(console, "MEM_ALLOC requires a size from 1 to 8192 bytes.");
        return true;
    }
    uint32_t slot = kApiMemoryBlocks;
    for (uint32_t index = 0; index < kApiMemoryBlocks; ++index) {
        if (!g_api_memory_blocks[index].used) { slot = index; break; }
    }
    if (slot == kApiMemoryBlocks) {
        add_history_line(console, "AP memory handle table is full.");
        return true;
    }

    for (uint32_t candidate = 0; candidate <= kApiMemoryBytes - requested; candidate += 8) {
        bool overlaps = false;
        uint32_t next_candidate = candidate + 8;
        for (uint32_t index = 0; index < kApiMemoryBlocks; ++index) {
            const ApiMemoryBlock& block = g_api_memory_blocks[index];
            if (!block.used) continue;
            const uint32_t candidate_end = candidate + requested;
            const uint32_t block_end = block.offset + block.size;
            if (candidate < block_end && block.offset < candidate_end) {
                overlaps = true;
                const uint32_t aligned_end = (block_end + 7u) & ~7u;
                if (aligned_end > next_candidate) next_candidate = aligned_end;
            }
        }
        if (overlaps) {
            candidate = next_candidate >= 8 ? next_candidate - 8 : candidate;
            continue;
        }
        g_api_memory_blocks[slot] = {candidate, requested, true};
        add_api_decimal(console, "AP memory handle: ", slot + 1);
        return true;
    }
    add_history_line(console, "AP memory arena is full.");
    return true;
}

bool api_invoke_service_v3(void* context, annyos::api::Service service,
                          const annyos::api::ServiceArguments* arguments) {
    auto& state = *static_cast<KernelApiContext*>(context);
    ConsoleState& console = *state.console;
    const char* first = api_argument(arguments, 0);
    const char* second = api_argument(arguments, 1);
    const char* third = api_argument(arguments, 2);
    switch (service) {
        case annyos::api::Service::sys_shutdown:
            add_history_line(console, "Shutting down AnnyOS...", g_prompt_color);
            redraw_history(*state.framebuffer, console, *state.working_directory);
            debug_write("AnnyOS: shutdown requested\r\n");
            if (state.boot_info->reset_system != nullptr) {
                state.boot_info->reset_system(2, 0, 0, nullptr);
            }
            for (;;) __asm__ volatile("cli; hlt" : : : "memory");
        case annyos::api::Service::sys_uptime:
            add_api_decimal(console, "Uptime: ", annyos::timer::ticks() / 1000);
            return true;
        case annyos::api::Service::sys_clear:
        case annyos::api::Service::ui_clear_screen:
            clear_history(console);
            redraw_history(*state.framebuffer, console, *state.working_directory);
            return true;
        case annyos::api::Service::sys_get_tick:
            add_api_decimal(console, "PIT ticks: ", annyos::timer::ticks());
            return true;
        case annyos::api::Service::sys_set_env:
            return api_environment_set(console, arguments);
        case annyos::api::Service::sys_get_env: {
            for (uint32_t index = 0; index < kEnvironmentSlots; ++index) {
                if (g_environment[index].used && equal_ignore_case(g_environment[index].key, first)) {
                    char line[96] = "";
                    uint32_t length = append_text(line, 0, sizeof(line), g_environment[index].key);
                    if (length + 1 < sizeof(line)) line[length++] = '=';
                    append_text(line, length, sizeof(line), g_environment[index].value);
                    add_history_line(console, line);
                    return true;
                }
            }
            add_history_line(console, "Environment key not found.");
            return true;
        }
        case annyos::api::Service::sys_cpu_info: {
            uint32_t eax = 0, ebx = 0, ecx = 0, edx = 0;
            __asm__ volatile("cpuid" : "+a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx));
            char vendor[32] = "CPU vendor: ";
            uint32_t length = 12;
            const uint32_t words[3] = {ebx, edx, ecx};
            for (uint32_t word = 0; word < 3; ++word) {
                for (uint32_t byte = 0; byte < 4; ++byte)
                    vendor[length++] = static_cast<char>(words[word] >> (byte * 8));
            }
            vendor[length] = '\0';
            add_history_line(console, vendor);
            return true;
        }
        case annyos::api::Service::sys_panic:
            debug_write("AnnyOS: AP requested kernel panic: ");
            debug_write(first);
            debug_write("\r\n");
            add_history_line(console, "Kernel panic requested by application.", g_help_title_color, true);
            redraw_history(*state.framebuffer, console, *state.working_directory);
            for (;;) __asm__ volatile("cli; hlt" : : : "memory");
        case annyos::api::Service::fs_create: {
            DriveState& drive = g_drives[state.working_directory->drive_index];
            if (!drive.mounted) { add_history_line(console, "FAT32 volume not found."); return true; }
            const uint32_t size = bounded_string_length(second, annyos::api::kMaximumServiceArgumentBytes);
            const auto* bytes = size == 0 ? nullptr : reinterpret_cast<const uint8_t*>(second);
            const uint32_t directory = state.working_directory->cluster_stack[state.working_directory->depth];
            add_history_line(console, annyos::storage::fat32_write_file(
                drive.volume, directory, first, bytes, size) ? "File created." : "FS_CREATE failed.");
            return true;
        }
        case annyos::api::Service::fs_read: {
            DriveState& drive = g_drives[state.working_directory->drive_index];
            annyos::storage::Fat32DirectoryEntry entry{};
            uint32_t bytes_read = 0;
            const uint32_t directory = state.working_directory->cluster_stack[state.working_directory->depth];
            if (!drive.mounted || !annyos::storage::fat32_find_file(drive.volume, directory, first, &entry) ||
                entry.size > kFileBufferCapacity || !annyos::storage::fat32_read_file(
                    drive.volume, entry, console.editor_buffer, kFileBufferCapacity, &bytes_read)) {
                add_history_line(console, "FS_READ failed or file exceeds 4 KiB.");
            } else if (!is_text_file(console.editor_buffer, bytes_read)) {
                add_history_line(console, "FS_READ: binary file; text output is unavailable.");
            } else {
                display_file_contents(console, console.editor_buffer, bytes_read);
            }
            return true;
        }
        case annyos::api::Service::fs_edit: {
            DriveState& drive = g_drives[state.working_directory->drive_index];
            if (!drive.mounted) add_history_line(console, "FAT32 volume not found.");
            else begin_file_edit(console, drive.volume,
                state.working_directory->cluster_stack[state.working_directory->depth], first);
            return true;
        }
        case annyos::api::Service::fs_delete: {
            DriveState& drive = g_drives[state.working_directory->drive_index];
            const uint32_t directory = state.working_directory->cluster_stack[state.working_directory->depth];
            add_history_line(console, drive.mounted && annyos::storage::fat32_delete_file(
                drive.volume, directory, first) ? "File deleted." : "FS_DELETE failed.");
            return true;
        }
        case annyos::api::Service::fs_exists: {
            DriveState& drive = g_drives[state.working_directory->drive_index];
            const uint32_t directory = state.working_directory->cluster_stack[state.working_directory->depth];
            annyos::storage::Fat32DirectoryEntry entry{};
            if (drive.mounted && annyos::storage::fat32_find_file(drive.volume, directory, first, &entry))
                add_history_line(console, "File exists.");
            else {
                uint32_t cluster = 0;
                add_history_line(console, drive.mounted && find_child_directory(
                    drive.volume, directory, first, &cluster) ? "Directory exists." : "Path not found.");
            }
            return true;
        }
        case annyos::api::Service::fs_list_dir: {
            DriveState& drive = g_drives[state.working_directory->drive_index];
            if (!drive.mounted) add_history_line(console, "FAT32 volume not found.");
            else list_directory(console, drive.volume, *state.working_directory, first);
            return true;
        }
        case annyos::api::Service::mem_alloc:
            return api_memory_allocate(console, arguments);
        case annyos::api::Service::mem_free: {
            uint32_t handle = 0;
            if (!parse_decimal(first, &handle) || handle == 0 || handle > kApiMemoryBlocks ||
                !g_api_memory_blocks[handle - 1].used) {
                add_history_line(console, "MEM_FREE requires a live allocation handle.");
            } else {
                g_api_memory_blocks[handle - 1].used = false;
                add_history_line(console, "AP memory block released.", g_prompt_color);
            }
            return true;
        }
        case annyos::api::Service::mem_total:
            add_memory_report(console, *state.boot_info);
            return true;
        case annyos::api::Service::mem_free_space: {
            DriveState& drive = g_drives[state.working_directory->drive_index];
            uint32_t free_clusters = 0;
            if (!drive.mounted || !annyos::storage::fat32_count_free_clusters(drive.volume, &free_clusters)) {
                add_history_line(console, "Could not query FAT32 free space.");
            } else {
                const uint64_t bytes = static_cast<uint64_t>(free_clusters) *
                    drive.volume.sectors_per_cluster * drive.volume.device.sector_size;
                add_api_decimal(console, "FAT32 free bytes: ", bytes);
            }
            return true;
        }
        case annyos::api::Service::mem_set: {
            uint32_t handle = 0;
            const uint32_t size = bounded_string_length(second, sizeof(g_api_memory));
            if (!parse_decimal(first, &handle) || handle == 0 || handle > kApiMemoryBlocks ||
                !g_api_memory_blocks[handle - 1].used || size > g_api_memory_blocks[handle - 1].size) {
                add_history_line(console, "MEM_SET requires HANDLE and text that fits the block.");
            } else {
                ApiMemoryBlock& block = g_api_memory_blocks[handle - 1];
                for (uint32_t index = 0; index < size; ++index)
                    g_api_memory[block.offset + index] = static_cast<uint8_t>(second[index]);
                add_history_line(console, "AP memory block updated.", g_prompt_color);
            }
            return true;
        }
        case annyos::api::Service::mem_copy: {
            uint32_t source = 0, destination = 0, amount = 0;
            if (!parse_decimal(first, &source) || !parse_decimal(second, &destination) ||
                !parse_decimal(third, &amount) || source == 0 || destination == 0 ||
                source > kApiMemoryBlocks || destination > kApiMemoryBlocks ||
                !g_api_memory_blocks[source - 1].used || !g_api_memory_blocks[destination - 1].used ||
                amount > g_api_memory_blocks[source - 1].size ||
                amount > g_api_memory_blocks[destination - 1].size) {
                add_history_line(console, "MEM_COPY requires source handle, destination handle, and bounded size.");
            } else {
                const ApiMemoryBlock& src = g_api_memory_blocks[source - 1];
                const ApiMemoryBlock& dst = g_api_memory_blocks[destination - 1];
                if (src.offset < dst.offset) {
                    for (uint32_t index = 0; index < amount; ++index)
                        g_api_memory[dst.offset + index] = g_api_memory[src.offset + index];
                } else if (src.offset > dst.offset) {
                    for (uint32_t index = amount; index > 0; --index)
                        g_api_memory[dst.offset + index - 1] = g_api_memory[src.offset + index - 1];
                } else {
                    // Same allocation: the requested copy is already satisfied.
                }
                add_history_line(console, "AP memory copied.", g_prompt_color);
            }
            return true;
        }
        case annyos::api::Service::disk_info: {
            const DriveState& drive = g_drives[state.working_directory->drive_index];
            if (!drive.ata_ready) add_history_line(console, "No ATA disk is active.");
            else {
                add_api_decimal(console, "Disk sectors: ", drive.block.sector_count);
                add_api_decimal(console, "Sector size: ", drive.block.sector_size);
                add_history_line(console, drive.mounted ? "Filesystem: FAT32" : "Filesystem: unmounted");
            }
            return true;
        }
        case annyos::api::Service::play_tone: {
            uint32_t frequency = 0, duration = 0;
            if (g_audio_volume == 0 || !parse_decimal(first, &frequency) ||
                !parse_decimal(second, &duration) || duration > 5000 ||
                !annyos::speaker::tone(frequency, duration)) {
                add_history_line(console, "PLAY_TONE requires frequency 20-20000 Hz and duration up to 5000 ms.");
            } else add_history_line(console, "Tone complete.", g_prompt_color);
            return true;
        }
        case annyos::api::Service::audio_stop:
            annyos::speaker::stop();
            add_history_line(console, "Speaker stopped.", g_prompt_color);
            return true;
        case annyos::api::Service::sound_effect:
            if (equal_ignore_case(first, "ERROR")) (void)annyos::speaker::tone(220, 100);
            else if (equal_ignore_case(first, "SUCCESS")) (void)annyos::speaker::tone(880, 80);
            else if (equal_ignore_case(first, "ALERT")) (void)annyos::speaker::tone(660, 160);
            else add_history_line(console, "SOUND_EFFECT supports SUCCESS, ERROR, and ALERT.");
            return true;
        case annyos::api::Service::speaker_init:
            add_history_line(console, "PC speaker is initialized by the first PLAY_TONE call.");
            return true;
        case annyos::api::Service::audio_volume: {
            uint32_t volume = 0;
            if (!parse_decimal(first, &volume) || volume > 100) {
                add_history_line(console, "AUDIO_VOLUME accepts 0 (mute) or 100 (full PC speaker output).");
            } else {
                g_audio_volume = volume == 0 ? 0 : 100;
                add_history_line(console, g_audio_volume == 0 ? "PC speaker muted." :
                                 "PC speaker unmuted; hardware has no amplitude control.", g_prompt_color);
            }
            return true;
        }
        case annyos::api::Service::ui_set_title:
            return api_write_title(context, first, static_cast<uint8_t>(bounded_string_length(
                first, annyos::api::kMaximumServiceArgumentBytes)));
        case annyos::api::Service::ui_print_text:
            return api_write_line(context, first, static_cast<uint8_t>(bounded_string_length(
                first, annyos::api::kMaximumServiceArgumentBytes)));
        case annyos::api::Service::ui_set_color:
            set_console_color(console, first);
            return true;
        case annyos::api::Service::ui_theme_dark:
            set_console_color(console, "MATRIX");
            return true;
        case annyos::api::Service::ui_theme_light:
            set_console_color(console, "BLUE");
            return true;
        case annyos::api::Service::ui_draw_pixel:
        case annyos::api::Service::ui_draw_rect: {
            uint32_t x = 0, y = 0, width = 1, height = 1, red = 0, green = 0, blue = 0;
            const bool rectangle = service == annyos::api::Service::ui_draw_rect;
            const char* width_arg = rectangle ? api_argument(arguments, 2) : "1";
            const char* height_arg = rectangle ? api_argument(arguments, 3) : "1";
            const uint32_t color_index = rectangle ? 4 : 2;
            const bool parsed = parse_decimal(first, &x) && parse_decimal(second, &y) &&
                parse_decimal(width_arg, &width) && parse_decimal(height_arg, &height) &&
                parse_decimal(api_argument(arguments, color_index), &red) &&
                parse_decimal(api_argument(arguments, color_index + 1), &green) &&
                parse_decimal(api_argument(arguments, color_index + 2), &blue);
            if (!parsed || red > 255 || green > 255 || blue > 255 ||
                x > static_cast<uint32_t>(INT32_MAX) || y > static_cast<uint32_t>(INT32_MAX) ||
                width > static_cast<uint32_t>(INT32_MAX) || height > static_cast<uint32_t>(INT32_MAX)) {
                add_history_line(console, rectangle
                    ? "UI_DRAW_RECT args: X Y WIDTH HEIGHT RED GREEN BLUE (0-255)."
                    : "UI_DRAW_PIXEL args: X Y RED GREEN BLUE (0-255).");
            } else {
                fill_rect(*state.framebuffer, static_cast<int32_t>(x), static_cast<int32_t>(y),
                    static_cast<int32_t>(width), static_cast<int32_t>(height),
                    {static_cast<uint8_t>(red), static_cast<uint8_t>(green), static_cast<uint8_t>(blue)});
            }
            return true;
        }
        case annyos::api::Service::ui_open_editor: {
            DriveState& drive = g_drives[state.working_directory->drive_index];
            if (!drive.mounted) add_history_line(console, "FAT32 volume not found.");
            else begin_file_edit(console, drive.volume,
                state.working_directory->cluster_stack[state.working_directory->depth], first);
            return true;
        }
        case annyos::api::Service::inp_read_key:
        case annyos::api::Service::inp_check_key: {
            const auto* record = g_driver_registry.query(annyos::drivers::ServiceId::keyboard_input, 1);
            const auto* keyboard = record == nullptr ? nullptr :
                static_cast<const annyos::drivers::service::KeyboardInputV1*>(record->interface_pointer);
            annyos::input::KeyEvent event{};
            if (keyboard == nullptr || !keyboard->poll(&event)) {
                add_history_line(console, "No key event available.");
            } else if (event.action == annyos::input::KeyAction::character) {
                char line[2] = {event.character, '\0'};
                if (service == annyos::api::Service::inp_check_key && *first != '\0' &&
                    first[0] != event.character) add_history_line(console, "Key does not match.");
                else add_history_line(console, line);
            } else add_history_line(console, "Keyboard event received.");
            return true;
        }
        case annyos::api::Service::serial_write:
            add_history_line(console, annyos::serial::write(first)
                ? "Serial text sent." : "Serial output unavailable.");
            return true;
        case annyos::api::Service::serial_read: {
            uint8_t byte = 0;
            if (!annyos::serial::read_byte(&byte)) add_history_line(console, "No serial byte available.");
            else {
                char line[2] = {byte >= 32 && byte <= 126 ? static_cast<char>(byte) : '?', '\0'};
                add_history_line(console, line);
            }
            return true;
        }
        case annyos::api::Service::usb_scan: {
            annyos::input::usb::ControllerInfo controller{};
            if (!annyos::input::usb::probe_xhci(&controller)) {
                add_history_line(console, "No xHCI controller found.");
            } else {
                add_history_line(console, "xHCI controller found; USB device enumeration is not active.");
            }
            return true;
        }
        case annyos::api::Service::time_get_rtc:
            add_rtc_report(console, true);
            add_rtc_report(console, false);
            return true;
        case annyos::api::Service::fs_mkdir:
        case annyos::api::Service::fs_rmdir:
        case annyos::api::Service::fs_copy:
        case annyos::api::Service::fs_move: {
            DriveState& drive = g_drives[state.working_directory->drive_index];
            const uint32_t directory = state.working_directory->cluster_stack[state.working_directory->depth];
            if (!drive.mounted) {
                add_history_line(console, "FAT32 volume not found.");
                return true;
            }
            bool success = false;
            const char* message = "FAT32 operation failed. Check 8.3 names and available space.";
            switch (service) {
                case annyos::api::Service::fs_mkdir:
                    success = annyos::storage::fat32_make_directory(drive.volume, directory, first);
                    message = success ? "Directory created." : message;
                    break;
                case annyos::api::Service::fs_rmdir:
                    success = annyos::storage::fat32_remove_directory(drive.volume, directory, first);
                    message = success ? "Empty directory removed." :
                        "Directory removal failed; it must be empty.";
                    break;
                case annyos::api::Service::fs_copy:
                    success = annyos::storage::fat32_copy_file(drive.volume, directory, first, second);
                    message = success ? "File copied." :
                        "Copy failed; files are limited to 4 KiB and destination must not exist.";
                    break;
                case annyos::api::Service::fs_move:
                    success = annyos::storage::fat32_move_file(drive.volume, directory, first, second);
                    message = success ? "File moved." :
                        "Move failed; files are limited to 4 KiB and destination must not exist.";
                    break;
                default: break;
            }
            add_history_line(console, message, success ? g_prompt_color : g_text_color);
            return true;
        }
        case annyos::api::Service::fs_mount_apfs:
        case annyos::api::Service::fs_mount_ntfs:
            add_history_line(console, "APFS/NTFS drivers currently probe metadata; they do not mount filesystems.");
            return true;
        case annyos::api::Service::disk_format:
            add_history_line(console, "Disk formatting is disabled; it requires a dedicated confirmed disk utility.");
            return true;
        case annyos::api::Service::ui_cursor_pos:
            add_history_line(console, "Cursor positioning is unavailable in the current text console.");
            return true;
        case annyos::api::Service::mouse_get_pos:
        case annyos::api::Service::mouse_check_click:
            add_history_line(console, "Mouse input is not registered by the current kernel.");
            return true;
        default:
            return api_invoke_service(context, service);
    }
}

annyos::api::AnnyOsApiV1 make_api(KernelApiContext& context) {
    return {sizeof(annyos::api::AnnyOsApiV1), annyos::api::kVersion, &context,
            api_write_line, api_write_title, api_write_help_row, api_invoke_service,
            api_invoke_service_v3};
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

const char* command_arguments(const char* input, const char* command) {
    uint32_t index = 0;
    while (command[index] != '\0') {
        if (input[index] == '\0' || lowercase(input[index]) != command[index]) return nullptr;
        ++index;
    }
    if (input[index] == '\0') return input + index;
    return input[index] == ' ' ? input + index + 1 : nullptr;
}

bool parse_file_arguments(const char* arguments, char filename[13], const char** content) {
    if (arguments == nullptr || filename == nullptr || content == nullptr) return false;
    while (*arguments == ' ') ++arguments;
    uint32_t length = 0;
    while (arguments[length] != '\0' && arguments[length] != ' ') {
        if (length + 1 >= 13) return false;
        filename[length] = arguments[length];
        ++length;
    }
    if (length == 0) return false;
    filename[length] = '\0';
    arguments += length;
    while (*arguments == ' ') ++arguments;
    *content = arguments;
    return true;
}

bool is_text_file(const uint8_t* data, uint32_t length) {
    for (uint32_t index = 0; index < length; ++index) {
        const uint8_t byte = data[index];
        if (byte != '\n' && byte != '\r' && byte != '\t' &&
            (byte < 32 || byte > 126)) return false;
    }
    return true;
}

void display_file_contents(ConsoleState& console, const uint8_t* data, uint32_t length) {
    if (length == 0) {
        add_history_line(console, "(empty file)");
        return;
    }
    char line[kLineCapacity];
    uint32_t line_length = 0;
    for (uint32_t index = 0; index < length; ++index) {
        const uint8_t byte = data[index];
        if (byte == '\r') continue;
        if (byte == '\n') {
            line[line_length] = '\0';
            add_history_line(console, line);
            line_length = 0;
            continue;
        }
        line[line_length++] = byte == '\t' ? ' ' : static_cast<char>(byte);
        if (line_length + 1 == kLineCapacity) {
            line[line_length] = '\0';
            add_history_line(console, line);
            line_length = 0;
        }
    }
    if (line_length != 0) {
        line[line_length] = '\0';
        add_history_line(console, line);
    }
}

bool read_text_file(ConsoleState& console, const annyos::storage::Fat32Volume& volume,
                    uint32_t directory_cluster, const char* filename,
                    uint32_t* bytes_read) {
    annyos::storage::Fat32DirectoryEntry entry{};
    if (!annyos::storage::fat32_find_file(volume, directory_cluster, filename, &entry)) {
        add_history_line(console, "File not found.");
        return false;
    }
    if (entry.size > kFileBufferCapacity) {
        add_history_line(console, "File is larger than the 4 KiB console limit.");
        return false;
    }
    if (!annyos::storage::fat32_read_file(volume, entry, console.editor_buffer,
                                           kFileBufferCapacity, bytes_read) ||
        *bytes_read != entry.size) {
        add_history_line(console, "Could not read file.");
        return false;
    }
    return true;
}

void begin_file_edit(ConsoleState& console, const annyos::storage::Fat32Volume& volume,
                     uint32_t directory_cluster, const char* filename) {
    annyos::storage::Fat32DirectoryEntry entry{};
    console.editor_length = 0;
    console.editor_cursor = 0;
    if (annyos::storage::fat32_find_file(volume, directory_cluster, filename, &entry)) {
        if (entry.size > kFileBufferCapacity) {
            add_history_line(console, "File is larger than the 4 KiB editor limit.");
            return;
        }
        uint32_t bytes_read = 0;
        if (!annyos::storage::fat32_read_file(volume, entry, console.editor_buffer,
                                               kFileBufferCapacity, &bytes_read) ||
            bytes_read != entry.size || !is_text_file(console.editor_buffer, bytes_read)) {
            add_history_line(console, "File cannot be opened as editable text.");
            return;
        }
        uint32_t normalized_length = 0;
        for (uint32_t index = 0; index < bytes_read; ++index) {
            if (console.editor_buffer[index] == '\r') continue;
            console.editor_buffer[normalized_length++] = console.editor_buffer[index];
        }
        console.editor_length = normalized_length;
        console.editor_cursor = normalized_length;
        const char loaded[] = "File loaded.";
        append_text(console.editor_status, 0, sizeof(console.editor_status), loaded);
    } else {
        const char created[] = "New file.";
        append_text(console.editor_status, 0, sizeof(console.editor_status), created);
    }

    uint32_t index = 0;
    while (filename[index] != '\0' && index + 1 < sizeof(console.editor_filename)) {
        console.editor_filename[index] = filename[index];
        ++index;
    }
    console.editor_filename[index] = '\0';
    console.editor_active = true;
}

uint32_t editor_line_start(const ConsoleState& console, uint32_t position) {
    uint32_t start = position > console.editor_length ? console.editor_length : position;
    while (start > 0 && console.editor_buffer[start - 1] != '\n') --start;
    return start;
}

uint32_t editor_line_end(const ConsoleState& console, uint32_t position) {
    uint32_t end = position > console.editor_length ? console.editor_length : position;
    while (end < console.editor_length && console.editor_buffer[end] != '\n') ++end;
    return end;
}

void editor_insert(ConsoleState& console, uint8_t byte) {
    if (console.editor_length >= kFileBufferCapacity) {
        append_text(console.editor_status, 0, sizeof(console.editor_status), "Buffer full (4 KiB).");
        return;
    }
    for (uint32_t index = console.editor_length; index > console.editor_cursor; --index) {
        console.editor_buffer[index] = console.editor_buffer[index - 1];
    }
    console.editor_buffer[console.editor_cursor++] = byte;
    ++console.editor_length;
    console.editor_status[0] = '\0';
}

void editor_erase(ConsoleState& console, uint32_t position) {
    if (position >= console.editor_length) return;
    for (uint32_t index = position; index + 1 < console.editor_length; ++index) {
        console.editor_buffer[index] = console.editor_buffer[index + 1];
    }
    --console.editor_length;
    if (console.editor_cursor > position) --console.editor_cursor;
    console.editor_status[0] = '\0';
}

void editor_move_vertical(ConsoleState& console, bool down) {
    const uint32_t start = editor_line_start(console, console.editor_cursor);
    const uint32_t end = editor_line_end(console, console.editor_cursor);
    const uint32_t column = console.editor_cursor - start;
    uint32_t target_start;
    uint32_t target_end;
    if (down) {
        if (end >= console.editor_length) return;
        target_start = end + 1;
        target_end = editor_line_end(console, target_start);
    } else {
        if (start == 0) return;
        target_end = start - 1;
        target_start = editor_line_start(console, target_end);
    }
    const uint32_t target_length = target_end - target_start;
    console.editor_cursor = target_start + (column < target_length ? column : target_length);
}

void finish_file_edit(ConsoleState& console, DriveState& drive,
                      const WorkingDirectory& working_directory, bool save) {
    if (save) {
        const uint8_t* data = console.editor_length == 0 ? nullptr : console.editor_buffer;
        if (!annyos::storage::fat32_write_file(
                drive.volume, working_directory.cluster_stack[working_directory.depth],
                console.editor_filename, data, console.editor_length)) {
            append_text(console.editor_status, 0, sizeof(console.editor_status), "Save failed; keep editing.");
            return;
        }
        add_history_line(console, "File saved.", g_prompt_color);
    } else {
        add_history_line(console, "Edit cancelled.");
    }
    console.editor_active = false;
    console.editor_status[0] = '\0';
}

void handle_editor_event(ConsoleState& console, DriveState& drive,
                         const WorkingDirectory& working_directory,
                         const annyos::input::KeyEvent& event) {
    switch (event.action) {
        case annyos::input::KeyAction::character:
            if (event.character >= 32 && event.character <= 126) {
                editor_insert(console, static_cast<uint8_t>(event.character));
            }
            break;
        case annyos::input::KeyAction::enter:
            editor_insert(console, '\n');
            break;
        case annyos::input::KeyAction::backspace:
            if (console.editor_cursor > 0) editor_erase(console, console.editor_cursor - 1);
            break;
        case annyos::input::KeyAction::delete_forward:
            editor_erase(console, console.editor_cursor);
            break;
        case annyos::input::KeyAction::left:
            if (console.editor_cursor > 0) --console.editor_cursor;
            break;
        case annyos::input::KeyAction::right:
            if (console.editor_cursor < console.editor_length) ++console.editor_cursor;
            break;
        case annyos::input::KeyAction::up:
            editor_move_vertical(console, false);
            break;
        case annyos::input::KeyAction::down:
            editor_move_vertical(console, true);
            break;
        case annyos::input::KeyAction::home:
            console.editor_cursor = editor_line_start(console, console.editor_cursor);
            break;
        case annyos::input::KeyAction::end:
            console.editor_cursor = editor_line_end(console, console.editor_cursor);
            break;
        case annyos::input::KeyAction::save:
            finish_file_edit(console, drive, working_directory, true);
            break;
        case annyos::input::KeyAction::escape:
            finish_file_edit(console, drive, working_directory, false);
            break;
    }
}

bool handle_file_command(ConsoleState& console, DriveState& drive,
                         const WorkingDirectory& working_directory) {
    const char* arguments = nullptr;
    enum class Operation : uint8_t { create, edit, read, remove };
    Operation operation;
    if ((arguments = command_arguments(console.input, "create")) != nullptr) {
        operation = Operation::create;
    } else if ((arguments = command_arguments(console.input, "edit")) != nullptr) {
        operation = Operation::edit;
    } else if ((arguments = command_arguments(console.input, "read")) != nullptr ||
               (arguments = command_arguments(console.input, "type")) != nullptr) {
        operation = Operation::read;
    } else if ((arguments = command_arguments(console.input, "delete")) != nullptr ||
               (arguments = command_arguments(console.input, "del")) != nullptr) {
        operation = Operation::remove;
    } else {
        return false;
    }

    if (!drive.mounted) {
        add_history_line(console, "FAT32 volume not found.");
        return true;
    }
    char filename[13];
    const char* content = nullptr;
    if (!parse_file_arguments(arguments, filename, &content)) {
        add_history_line(console, "Usage: CREATE|EDIT|READ|DELETE <8.3 FILE>");
        return true;
    }
    const uint32_t directory_cluster =
        working_directory.cluster_stack[working_directory.depth];

    switch (operation) {
        case Operation::create: {
            annyos::storage::Fat32DirectoryEntry existing{};
            if (annyos::storage::fat32_find_file(drive.volume, directory_cluster,
                                                  filename, &existing)) {
                add_history_line(console, "File already exists; use EDIT to replace it.");
                return true;
            }
            const uint32_t content_length = bounded_string_length(content, kInputCapacity);
            const uint8_t* data = content_length == 0 ? nullptr :
                reinterpret_cast<const uint8_t*>(content);
            if (content_length >= kInputCapacity ||
                !annyos::storage::fat32_write_file(drive.volume, directory_cluster,
                                                    filename, data, content_length)) {
                add_history_line(console, "Create failed. Use a valid 8.3 name and writable volume.");
            } else {
                add_history_line(console, "File created.", g_prompt_color);
            }
            return true;
        }
        case Operation::edit:
            if (*content != '\0') {
                add_history_line(console, "Use EDIT <FILE> to open the editor; put text on following lines.");
            } else {
                begin_file_edit(console, drive.volume, directory_cluster, filename);
            }
            return true;
        case Operation::read: {
            if (*content != '\0') {
                add_history_line(console, "Usage: READ <FILE>");
                return true;
            }
            uint32_t bytes_read = 0;
            if (read_text_file(console, drive.volume, directory_cluster, filename, &bytes_read)) {
                if (!is_text_file(console.editor_buffer, bytes_read)) {
                    add_history_line(console, "File contains binary data; text display is unavailable.");
                } else {
                    add_history_line(console, "FILE CONTENT", g_help_title_color, true);
                    display_file_contents(console, console.editor_buffer, bytes_read);
                }
            }
            return true;
        }
        case Operation::remove:
            if (*content != '\0') {
                add_history_line(console, "Usage: DELETE <FILE>");
            } else if (annyos::storage::fat32_delete_file(
                           drive.volume, directory_cluster, filename)) {
                add_history_line(console, "File deleted.", g_prompt_color);
            } else {
                add_history_line(console, "Delete failed; file may be missing, read-only, or a folder.");
            }
            return true;
    }
    return true;
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

bool handle_configsave_command(ConsoleState& console, const char* arguments) {
    while (*arguments == ' ') ++arguments;
    if (*arguments == '\0' || command_equals(arguments, "list")) {
        add_history_line(console, "ANNYOS | PERSISTENT SETTINGS", g_help_title_color, true);
        char line[40] = "THEME=";
        append_text(line, 6, sizeof(line),
                    annyos::config::config_save_active_theme(&g_config_save));
        add_history_line(console, line, g_prompt_color);
        add_history_line(console, "File: C:\\ANNYOS.CFG");
        add_history_line(console, "Use CONFIGSAVE GET THEME or SET THEME <VALUE>.");
        return true;
    }

    if (command_starts_with(arguments, "get ")) {
        const char* key = arguments + 4;
        while (*key == ' ') ++key;
        const char* end = key;
        while (*end != '\0' && *end != ' ') ++end;
        if (end == key || *end != '\0') {
            add_history_line(console, "Usage: CONFIGSAVE GET THEME");
        } else if (!equal_ignore_case(key, "theme")) {
            add_history_line(console, "Unknown setting. Supported key: THEME");
        } else {
            char line[40] = "THEME=";
            append_text(line, 6, sizeof(line),
                        annyos::config::config_save_active_theme(&g_config_save));
            add_history_line(console, line, g_prompt_color);
        }
        return true;
    }

    if (command_starts_with(arguments, "set ")) {
        const char* cursor = arguments + 4;
        while (*cursor == ' ') ++cursor;
        char key[16]{};
        uint32_t key_length = 0;
        while (*cursor != '\0' && *cursor != ' ' && *cursor != '=' &&
               key_length + 1 < sizeof(key)) {
            key[key_length++] = *cursor++;
        }
        key[key_length] = '\0';
        if (*cursor == '=') ++cursor;
        while (*cursor == ' ') ++cursor;
        char value[16]{};
        uint32_t value_length = 0;
        while (*cursor != '\0' && *cursor != ' ' && value_length + 1 < sizeof(value)) {
            value[value_length++] = *cursor++;
        }
        value[value_length] = '\0';
        while (*cursor == ' ') ++cursor;
        if (key_length == 0 || value_length == 0 || *cursor != '\0') {
            add_history_line(console, "Usage: CONFIGSAVE SET THEME <VALUE>");
        } else if (!equal_ignore_case(key, "theme")) {
            add_history_line(console, "Unknown setting. Supported key: THEME");
        } else {
            set_console_color(console, value);
        }
        return true;
    }

    add_history_line(console, "Usage: CONFIGSAVE [LIST | GET THEME | SET THEME <VALUE>]");
    return true;
}

void list_directory(ConsoleState& console, const annyos::storage::Fat32Volume& volume,
                    const WorkingDirectory& current_directory, const char* requested_path) {
    WorkingDirectory target_directory = current_directory;
    while (*requested_path == ' ') ++requested_path;
    if (*requested_path != '\0' &&
        !change_directory(&target_directory, volume, requested_path)) {
        add_history_line(console, "Directory not found. Usage: DIR [PATH]");
        return;
    }

    char directory_line[kLineCapacity] = "DIRECTORY  ";
    const uint32_t directory_length = append_text(directory_line, 11, sizeof(directory_line),
                                                   target_directory.path);
    directory_line[directory_length] = '\0';
    add_history_line(console, directory_line, g_help_title_color, true);
    add_history_line(console, "------------------------------------------------");
    add_directory_row(console, "NAME", "TYPE", "SIZE", g_prompt_color, true);

    DirectoryListing listing{&console, 0, 0, 0};
    if (!annyos::storage::fat32_list_directory(
            volume, target_directory.cluster_stack[target_directory.depth],
            append_fat32_entry, &listing)) {
        add_history_line(console, "Error reading directory.");
        return;
    }

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
        } else if (command_equals(console.input, "configsave")) {
            handle_configsave_command(console, "");
        } else if (command_starts_with(console.input, "configsave ")) {
            handle_configsave_command(console, console.input + 11);
        } else if (handle_file_command(console, active_drive, *working_directory)) {
            // File commands are handled against the active FAT32 directory.
        } else if (const char* dir_path = command_arguments(console.input, "dir")) {
            if (!fat32_mounted) {
                add_history_line(console, "FAT32 volume not found.");
            } else {
                list_directory(console, fat32_volume, *working_directory, dir_path);
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

void draw_editor(const annyos::FramebufferInfo& fb, const ConsoleState& console) {
    const int32_t width = static_cast<int32_t>(fb.width);
    const int32_t height = static_cast<int32_t>(fb.height);
    const Color header_color{0, 0, 96};
    fill_rect(fb, 0, 0, width, height, g_screen_color);
    fill_rect(fb, 0, 0, width, 48, header_color);
    fill_rect(fb, 0, height - 52, width, 52, header_color);
    draw_text(fb, kMargin, 12, "ANNYOS TEXT EDITOR", g_help_title_color, true);
    const int32_t filename_x = draw_text(fb, kMargin + 225, 12, console.editor_filename,
                                         g_input_color, true);
    char size_text[32] = "  ";
    uint32_t size_length = 2;
    append_decimal(size_text, &size_length, console.editor_length);
    append_text(size_text, size_length, sizeof(size_text), " / 4096 bytes");
    draw_text(fb, filename_x + 12, 12, size_text, g_text_color);

    const int32_t body_top = 60;
    const int32_t footer_top = height - 52;
    const int32_t body_height = footer_top - body_top;
    const int32_t rows = body_height / kLineHeight > 0 ? body_height / kLineHeight : 1;
    uint32_t cursor_line = 0;
    for (uint32_t index = 0; index < console.editor_cursor; ++index) {
        if (console.editor_buffer[index] == '\n') ++cursor_line;
    }
    const uint32_t first_line = cursor_line >= static_cast<uint32_t>(rows)
        ? cursor_line - static_cast<uint32_t>(rows) + 1 : 0;
    uint32_t line_number = 0;
    uint32_t line_start = 0;
    int32_t row = 0;
    while (line_start <= console.editor_length && row < rows) {
        const uint32_t line_end = editor_line_end(console, line_start);
        if (line_number >= first_line) {
            const int32_t y = body_top + row * kLineHeight;
            char number[12] = "";
            uint32_t number_length = 0;
            append_decimal(number, &number_length, line_number + 1);
            draw_text(fb, kMargin, y, number, g_prompt_color);

            uint32_t visible_start = line_start;
            if (line_number == cursor_line) {
                const uint32_t cursor_column = console.editor_cursor - line_start;
                const uint32_t horizontal_offset = cursor_column >= kInputCapacity - 4
                    ? cursor_column - (kInputCapacity - 5) : 0;
                visible_start += horizontal_offset;
            }
            char visible[kInputCapacity] = "";
            char cursor_prefix[kInputCapacity] = "";
            uint32_t visible_length = 0;
            uint32_t prefix_length = 0;
            for (uint32_t index = visible_start;
                 index < line_end && visible_length + 1 < sizeof(visible); ++index) {
                const char character = console.editor_buffer[index] == '\t'
                    ? ' ' : static_cast<char>(console.editor_buffer[index]);
                visible[visible_length++] = character;
                if (line_number == cursor_line && index < console.editor_cursor) {
                    cursor_prefix[prefix_length++] = character;
                }
            }
            visible[visible_length] = '\0';
            cursor_prefix[prefix_length] = '\0';
            const int32_t text_x = kMargin + 38;
            draw_text(fb, text_x, y, visible, g_text_color);
            if (line_number == cursor_line) {
                const int32_t cursor_x = text_x + measure_text(cursor_prefix);
                fill_rect(fb, cursor_x, y + 18, 8, 2, g_input_color);
            }
            ++row;
        }
        if (line_end == console.editor_length) break;
        line_start = line_end + 1;
        ++line_number;
    }

    draw_text(fb, kMargin, footer_top + 5,
              "F2 Save     ESC Cancel     Arrows Move     HOME/END Line",
              g_prompt_color, true);
    draw_text(fb, kMargin, footer_top + 27,
              "ENTER New line     BACKSPACE/Delete Edit text",
              g_text_color);
    if (console.editor_status[0] != '\0') {
        const int32_t status_x = measure_text("F2 Save     ESC Cancel     Arrows Move     HOME/END Line") +
                                 2 * kMargin;
        draw_text(fb, status_x, footer_top + 5, console.editor_status, g_help_title_color);
    }
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
    if (console.editor_active) {
        draw_editor(fb, console);
        return;
    }
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
    if (console.editor_active) {
        draw_editor(fb, console);
        return;
    }
    const int32_t history_rows = visible_rows(fb) - 1;
    const uint32_t visible_history = console.history_count < static_cast<uint32_t>(history_rows)
        ? console.history_count : static_cast<uint32_t>(history_rows);
    draw_prompt(fb, console, working_directory, static_cast<int32_t>(visible_history));
}

} // namespace

extern "C" [[noreturn]] void kernel_main(const annyos::BootInfo* boot_info) {
    __asm__ volatile("cli" : : : "memory");
    initialize_kernel_drivers();
    annyos::input::usb::ControllerInfo usb_controller{};
    const bool xhci_found = annyos::input::usb::probe_xhci(&usb_controller);
    if (xhci_found) {
        debug_write("AnnyOS: xHCI PCI function detected at ");
        char pci_location[48] = "bus ";
        uint32_t pci_location_length = 4;
        append_decimal(pci_location, &pci_location_length, usb_controller.bus);
        pci_location_length = append_text(pci_location, pci_location_length,
                                          sizeof(pci_location), ", device ");
        append_decimal(pci_location, &pci_location_length, usb_controller.device);
        pci_location_length = append_text(pci_location, pci_location_length,
                                          sizeof(pci_location), ", function ");
        append_decimal(pci_location, &pci_location_length, usb_controller.function);
        debug_write(pci_location);
        debug_write("; MMIO BAR 0x");
        debug_write_hex(usb_controller.mmio_base);
        debug_write(". MMIO mapping and HID endpoint enumeration are not active yet.\r\n");
    } else {
        debug_write("AnnyOS: no usable xHCI PCI controller found; class matches: ");
        char match_count[8] = {};
        uint32_t match_count_length = 0;
        append_decimal(match_count, &match_count_length, usb_controller.pci_xhci_matches);
        debug_write(match_count);
        debug_write("\r\n");
    }
    if (boot_info == nullptr || boot_info->framebuffer.address == nullptr ||
        boot_info->framebuffer.width == 0 || boot_info->framebuffer.height == 0) {
        for (;;) __asm__ volatile("hlt");
    }

    annyos::config::config_save_initialize(
        &g_config_save, g_drives[0].mounted ? &g_drives[0].volume : nullptr);
    apply_console_theme(annyos::config::config_save_active_theme(&g_config_save));
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
        add_history_line(console, "AnnyOS Command Processor [Version 0.4.4]");
        add_history_line(console, "Type HELP for commands.");
    }
    redraw_history(boot_info->framebuffer, console, working_directory);
    debug_write("AnnyOS: DOS-style command console ready\r\n");
    const annyos::drivers::ServiceRecord* keyboard_record =
        g_driver_registry.query(annyos::drivers::ServiceId::keyboard_input, 1);
    const auto* keyboard = keyboard_record == nullptr ? nullptr :
        static_cast<const annyos::drivers::service::KeyboardInputV1*>(
            keyboard_record->interface_pointer);

    for (;;) {
        annyos::input::KeyEvent event{};
        if (keyboard == nullptr || !keyboard->poll(&event)) {
            __asm__ volatile("pause");
            continue;
        }

        if (console.editor_active) {
            handle_editor_event(console, g_drives[working_directory.drive_index],
                                working_directory, event);
            if (console.editor_active) {
                draw_editor(boot_info->framebuffer, console);
            } else {
                redraw_history(boot_info->framebuffer, console, working_directory);
            }
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
