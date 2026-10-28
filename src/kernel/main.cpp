#include "annyos/boot_info.hpp"
#include "annyos/api.hpp"
#include "annyos/ata_pio.hpp"
#include "annyos/ap_format.hpp"
#include "annyos/config_save.hpp"
#include "annyos/curl_host_bridge.hpp"
#include "annyos/driver_registry.hpp"
#include "annyos/driver_services.hpp"
#include "annyos/e1000.hpp"
#include "annyos/apfs.hpp"
#include "annyos/fat32.hpp"
#include "annyos/ntfs.hpp"
#include "annyos/pc_speaker.hpp"
#include "annyos/ps2_keyboard.hpp"
#include "annyos/serial.hpp"
#include "annyos/timer_pit.hpp"
#include "annyos/usb_hid.hpp"
#include "annyos/virtio_net.hpp"
#include "annyos/network_stack.hpp"
#include "shell_state.hpp"

namespace {

using namespace annyos::shell;

constexpr uint32_t kAtaDriveSlots = 4;
constexpr uint32_t kApBufferCapacity = annyos::application::kApMaximumFileBytes;
constexpr uint32_t kApiMemoryBytes = 8192;
constexpr uint32_t kApiMemoryBlocks = 16;
constexpr uint32_t kEnvironmentSlots = 16;

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
    WorkingDirectory* working_directory;
};

ConsoleState g_console;
WorkingDirectory g_working_directory;
DriveState g_drives[kAtaDriveSlots];
annyos::config::ConfigSave g_config_save{};
uint8_t g_ap_image[kApBufferCapacity];
annyos::curl::HostResponse g_curl_response{};
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

ThemeColors current_theme_colors() {
    return {g_screen_color, g_text_color, g_prompt_color, g_input_color,
            g_help_title_color, g_monochrome_theme};
}

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
enum class EthernetBackend : uint8_t { none, virtio, e1000 };
EthernetBackend g_ethernet_backend = EthernetBackend::none;
bool ethernet_ready() {
    if (g_ethernet_backend == EthernetBackend::virtio) return annyos::network::virtio_net_ready();
    if (g_ethernet_backend == EthernetBackend::e1000) return annyos::network::e1000_ready();
    return false;
}
annyos::network::MacAddress ethernet_mac_address() {
    if (g_ethernet_backend == EthernetBackend::virtio)
        return annyos::network::virtio_net_mac_address();
    if (g_ethernet_backend == EthernetBackend::e1000)
        return annyos::network::e1000_mac_address();
    return {};
}
bool ethernet_send(const uint8_t* frame, uint16_t length) {
    if (g_ethernet_backend == EthernetBackend::virtio)
        return annyos::network::virtio_net_send(frame, length);
    if (g_ethernet_backend == EthernetBackend::e1000)
        return annyos::network::e1000_send(frame, length);
    return false;
}
uint32_t ethernet_poll(annyos::network::ReceiveFrame receive, void* context, uint32_t budget) {
    if (g_ethernet_backend == EthernetBackend::virtio)
        return annyos::network::virtio_net_poll(receive, context, budget);
    if (g_ethernet_backend == EthernetBackend::e1000)
        return annyos::network::e1000_poll(receive, context, budget);
    return 0;
}
const annyos::drivers::service::EthernetFramesV1 g_ethernet_service{
    ethernet_ready, ethernet_mac_address, ethernet_send, ethernet_poll};

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

bool initialize_ethernet_driver(void* context, annyos::drivers::DriverRegistry& registry) {
    if (context == nullptr) return false;
    const auto& boot_info = *static_cast<const annyos::BootInfo*>(context);
    if (annyos::network::virtio_net_initialize(boot_info)) {
        g_ethernet_backend = EthernetBackend::virtio;
        debug_write("AnnyOS: legacy virtio-net link initialized\r\n");
    } else if (annyos::network::e1000_initialize(boot_info)) {
        g_ethernet_backend = EthernetBackend::e1000;
        debug_write("AnnyOS: Intel 82540EM e1000 link initialized\r\n");
    } else {
        g_ethernet_backend = EthernetBackend::none;
        debug_write("AnnyOS: no supported Ethernet controller found\r\n");
        return false;
    }
    return registry.publish(annyos::drivers::ServiceId::ethernet_frames, 1,
                            &g_ethernet_service);
}

void initialize_kernel_drivers(const annyos::BootInfo* boot_info) {
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
        {DriverId::pci_ethernet, "network.pci-ethernet", 1, nullptr, 0, initialize_ethernet_driver},
    };

    g_driver_registry.reset();
    for (const DriverDescriptor& descriptor : descriptors) {
        if (!g_driver_registry.register_driver(descriptor)) {
            debug_write("AnnyOS: driver registry rejected a built-in driver\r\n");
        }
    }
    const InitializationSummary summary = g_driver_registry.initialize_all(
        const_cast<annyos::BootInfo*>(boot_info));
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
    add_help_row(console, "DISKDEMO", "Show the disk API report");
    add_help_row(console, "CREATE <FILE>", "Create empty file or use CREATE <FILE> TEXT");
    add_help_row(console, "EDIT <FILE>", "Arrows edit text; F2 saves, ESC cancels");
    add_help_row(console, "READ <FILE>", "Print a file's text contents");
    add_help_row(console, "DELETE <FILE>", "Delete a file and release its clusters");
    add_help_row(console, "CLS", "Clear the screen");
    add_help_row(console, "ECHO <TEXT>", "Print a message");
    add_help_row(console, "CURL [OPTIONS]", "HTTP(S): -X -d -H -o/-O -L -v -s -u -k");
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

void attempt_reboot(const annyos::BootInfo& boot_info) {
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

}

[[noreturn]] void show_reboot_animation(const annyos::FramebufferInfo& framebuffer,
                                        const annyos::BootInfo& boot_info) {
    constexpr char kSpinner[] = {'/', '|', '\\', '_'};
    constexpr char kMessage[] = " Rebooting";
    char line[sizeof(kMessage) + 1];
    __asm__ volatile("cli" : : : "memory");
    for (;;) {
        for (uint32_t frame = 0; frame < sizeof(kSpinner); ++frame) {
            line[0] = kSpinner[frame];
            uint32_t length = 1;
            for (uint32_t index = 0; kMessage[index] != '\0'; ++index)
                line[length++] = kMessage[index];
            line[length] = '\0';

            annyos::shell::ui::fill_rect(framebuffer, 0, 0, static_cast<int32_t>(framebuffer.width),
                      static_cast<int32_t>(framebuffer.height), g_screen_color);
            const int32_t rendered_width = annyos::shell::ui::text_width(line);
            const int32_t x = (static_cast<int32_t>(framebuffer.width) - rendered_width) / 2;
            const int32_t y = (static_cast<int32_t>(framebuffer.height) - kLineHeight) / 2;
            annyos::shell::ui::draw_text(framebuffer, x, y, line, g_prompt_color, true);
            (void)annyos::timer::sleep_ms(120);
        }
        // Keep the spinner alive if firmware or chipset reset requests return.
        // Normal UEFI ResetSystem and hardware reset paths do not return.
        attempt_reboot(boot_info);
    }
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

void list_directory(ConsoleState& console, const annyos::storage::Fat32Volume& volume,
                    const WorkingDirectory& current_directory, const char* requested_path);
bool parse_disk_argument(const char* text, uint8_t default_index, uint8_t* index);
void list_disks(ConsoleState& console, const DriveState* drives);

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
            add_history_line(*state.console, "AnnyOS Version 0.4.8");
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
            debug_write("AnnyOS: reboot requested\r\n");
            show_reboot_animation(*state.framebuffer, *state.boot_info);
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

void run_curl_command(ConsoleState& console, DriveState& drive,
                      const WorkingDirectory& working_directory,
                      const annyos::api::ServiceArguments* arguments);

bool api_invoke_service_v3(void* context, annyos::api::Service service,
                          const annyos::api::ServiceArguments* arguments) {
    auto& state = *static_cast<KernelApiContext*>(context);
    ConsoleState& console = *state.console;
    const char* first = api_argument(arguments, 0);
    const char* second = api_argument(arguments, 1);
    const char* third = api_argument(arguments, 2);
    switch (service) {
        case annyos::api::Service::curl: {
            WorkingDirectory& working_directory = *state.working_directory;
            DriveState& drive = g_drives[working_directory.drive_index];
            run_curl_command(console, drive, working_directory, arguments);
            return true;
        }
        case annyos::api::Service::sys_shutdown:
            add_history_line(console, "Shutting down AnnyOS...", g_prompt_color);
            annyos::shell::ui::redraw_history(*state.framebuffer, console, *state.working_directory, current_theme_colors());
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
            annyos::shell::ui::redraw_history(*state.framebuffer, console, *state.working_directory, current_theme_colors());
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
            annyos::shell::ui::redraw_history(*state.framebuffer, console, *state.working_directory, current_theme_colors());
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
            uint8_t index = state.working_directory->drive_index;
            if (!parse_disk_argument(first, index, &index)) {
                add_history_line(console, "DISK_INFO accepts optional drive C:, D:, E:, or F:.");
                return true;
            }
            const DriveState& drive = g_drives[index];
            if (!drive.ata_ready) add_history_line(console, "No ATA disk detected for that drive.");
            else {
                char label[] = "Disk X: details";
                label[5] = static_cast<char>('C' + index);
                add_history_line(console, label, g_help_title_color, true);
                add_api_decimal(console, "Disk sectors: ", drive.block.sector_count);
                add_api_decimal(console, "Sector size: ", drive.block.sector_size);
                add_history_line(console, drive.mounted ? "Filesystem: FAT32 mounted" :
                                 "Filesystem: not mounted by AnnyOS");
            }
            return true;
        }
        case annyos::api::Service::disk_list:
            list_disks(console, g_drives);
            return true;
        case annyos::api::Service::disk_space: {
            uint8_t index = state.working_directory->drive_index;
            if (!parse_disk_argument(first, index, &index)) {
                add_history_line(console, "DISK_SPACE accepts optional drive C:, D:, E:, or F:.");
                return true;
            }
            const DriveState& drive = g_drives[index];
            uint32_t free_clusters = 0;
            if (!drive.mounted || !annyos::storage::fat32_count_free_clusters(
                    drive.volume, &free_clusters)) {
                add_history_line(console, "Free space requires a mounted FAT32 volume.");
            } else {
                const uint64_t cluster_bytes = static_cast<uint64_t>(
                    drive.volume.sectors_per_cluster) * drive.block.sector_size;
                add_api_decimal(console, "FAT32 free bytes: ",
                    static_cast<uint64_t>(free_clusters) * cluster_bytes);
                add_api_decimal(console, "FAT32 total bytes: ",
                    static_cast<uint64_t>(drive.volume.cluster_count) * cluster_bytes);
            }
            return true;
        }
        case annyos::api::Service::disk_use: {
            uint8_t index = 0;
            if (!parse_disk_argument(first, state.working_directory->drive_index, &index) ||
                !g_drives[index].mounted) {
                add_history_line(console, "DISK_USE requires a mounted drive: C:, D:, E:, or F:.");
            } else {
                reset_working_directory(*state.working_directory, g_drives[index].volume,
                                        static_cast<char>('C' + index), index);
                char line[kLineCapacity] = "Current directory: ";
                const uint32_t length = append_text(line, 19, sizeof(line),
                                                    state.working_directory->path);
                line[length] = '\0';
                add_history_line(console, line, g_prompt_color);
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
                annyos::shell::ui::fill_rect(*state.framebuffer, static_cast<int32_t>(x), static_cast<int32_t>(y),
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

bool parse_disk_argument(const char* text, uint8_t default_index, uint8_t* index) {
    if (text == nullptr || index == nullptr || default_index >= kAtaDriveSlots) return false;
    while (*text == ' ') ++text;
    if (*text == '\0') {
        *index = default_index;
        return true;
    }
    const char letter = uppercase(text[0]);
    if (letter < 'C' || letter > 'F' || text[1] != ':') return false;
    text += 2;
    while (*text == ' ') ++text;
    if (*text != '\0') return false;
    *index = static_cast<uint8_t>(letter - 'C');
    return true;
}

void list_disks(ConsoleState& console, const DriveState* drives) {
    add_history_line(console, "ANNYOS  |  DISK DEVICES", g_help_title_color, true);
    // Keep headings and values short enough for the proportional framebuffer
    // table's fixed column positions (name, device, size/filesystem).
    add_history_line(console, "DISK\tBUS\tSIZE / FS", g_prompt_color, true);
    uint32_t found = 0;
    for (uint8_t index = 0; index < kAtaDriveSlots; ++index) {
        const DriveState& drive = drives[index];
        if (!drive.ata_ready) continue;
        char name[3] = {static_cast<char>('C' + index), ':', '\0'};
        // Keep the middle column short enough to fit before the fixed capacity
        // column in the framebuffer table renderer.
        const char* interface_name = drive.ata.io_base == 0x1f0
            ? (drive.ata.drive == 0 ? "ATA P-M" : "ATA P-S")
            : (drive.ata.drive == 0 ? "ATA S-M" : "ATA S-S");
        char capacity[24] = "";
        uint32_t length = 0;
        const uint64_t bytes = static_cast<uint64_t>(drive.block.sector_count) *
                               drive.block.sector_size;
        append_decimal(capacity, &length, bytes / (1024 * 1024));
        append_text(capacity, length, sizeof(capacity), " MiB ");
        const uint32_t capacity_length = bounded_string_length(capacity, sizeof(capacity));
        append_text(capacity, capacity_length, sizeof(capacity),
                    drive.mounted ? "FAT32" : "unmounted");
        add_directory_row(console, name, interface_name, capacity, g_text_color);
        ++found;
    }
    if (found == 0) add_history_line(console, "No ATA disks detected.");
    add_directory_row(console, "USB", "USB MSC", "not enumerated", g_help_title_color);
    add_history_line(console,
        "USB storage is not mounted. USB MSC driver support is required for FAT32 access.",
        g_help_title_color);
}

void print_curl_text(ConsoleState& console, const uint8_t* bytes, uint32_t byte_count) {
    char line[kLineCapacity];
    uint32_t length = 0;
    for (uint32_t index = 0; index < byte_count; ++index) {
        const uint8_t byte = bytes[index];
        if (byte == '\r') continue;
        if (byte == '\n') {
            line[length] = '\0';
            add_history_line(console, line);
            length = 0;
            continue;
        }
        if (byte == '\t') {
            if (length + 1 < sizeof(line)) line[length++] = ' ';
        } else if (byte >= 32 && byte <= 126) {
            if (length + 1 < sizeof(line)) line[length++] = static_cast<char>(byte);
        } else if (byte >= 128) {
            if (length + 1 < sizeof(line)) line[length++] = '?';
        }
        if (length + 1 >= sizeof(line)) {
            line[length] = '\0';
            add_history_line(console, line);
            length = 0;
        }
    }
    if (length != 0) {
        line[length] = '\0';
        add_history_line(console, line);
    }
}

bool normalize_curl_filename(const char* source, char destination[13]) {
    if (source == nullptr || destination == nullptr) return false;
    uint32_t base_length = 0;
    uint32_t extension_length = 0;
    uint32_t output_length = 0;
    bool after_dot = false;
    while (source[output_length] != '\0') {
        const char value = source[output_length];
        if (value == '.') {
            if (after_dot || base_length == 0) return false;
            after_dot = true;
            destination[output_length++] = '.';
            continue;
        }
        const char normalized = uppercase(value);
        if (!((normalized >= 'A' && normalized <= 'Z') ||
              (normalized >= '0' && normalized <= '9') || normalized == '_' ||
              normalized == '-' || normalized == '$' || normalized == '~')) return false;
        if (!after_dot) {
            if (++base_length > 8) return false;
        } else if (++extension_length > 3) {
            return false;
        }
        if (output_length >= 12) return false;
        destination[output_length++] = normalized;
    }
    destination[output_length] = '\0';
    return base_length != 0 && (!after_dot || extension_length != 0);
}

bool append_curl_wire_arguments(
                                const char tokens[annyos::api::kMaximumServiceArguments]
                                                   [annyos::api::kMaximumServiceArgumentBytes + 1],
                                uint32_t token_count,
                                char* output, uint32_t capacity) {
    if (tokens == nullptr || output == nullptr || capacity == 0) return false;
    uint32_t length = 0;
    output[0] = '\0';
    for (uint32_t token_index = 0; token_index < token_count; ++token_index) {
        if (token_index != 0) {
            if (length + 1 >= capacity) return false;
            output[length++] = ' ';
        }
        if (length + 1 >= capacity) return false;
        output[length++] = '"';
        for (const char* cursor = tokens[token_index]; *cursor != '\0'; ++cursor) {
            if (*cursor == '"' || *cursor == '\\') {
                if (length + 1 >= capacity) return false;
                output[length++] = '\\';
            }
            if (length + 1 >= capacity) return false;
            output[length++] = *cursor;
        }
        if (length + 1 >= capacity) return false;
        output[length++] = '"';
        output[length] = '\0';
    }
    return length != 0;
}

bool curl_token_equals(const char* token, const char* expected) {
    if (token == nullptr || expected == nullptr) return false;
    while (*token != '\0' && *expected != '\0') {
        if (*token++ != *expected++) return false;
    }
    return *token == '\0' && *expected == '\0';
}

void add_curl_help(ConsoleState& console) {
    add_history_line(console, "ANNYOS | CURL GUIDE", g_help_title_color, true);
    add_help_row(console, "SYNTAX", "CURL [OPTIONS] URL");
    add_help_row(console, "-X METHOD", "HTTP method, for example GET or POST");
    add_help_row(console, "-d DATA", "Request body; commonly used with POST");
    add_help_row(console, "-H HEADER", "Add an HTTP header");
    add_help_row(console, "-u USER:PASS", "HTTP basic authentication");
    add_help_row(console, "-o FILE", "Save response in current FAT32 folder (8.3 name)");
    add_help_row(console, "-o -", "Print response in the console");
    add_help_row(console, "-O", "Save using the URL filename (must fit FAT 8.3)");
    add_help_row(console, "-L", "Follow redirects");
    add_help_row(console, "-v", "Show request diagnostics");
    add_help_row(console, "-s", "Suppress curl diagnostics");
    add_help_row(console, "-k", "Skip TLS certificate verification");
    add_help_row(console, "-h, --help", "Show this guide");
    add_history_line(console, "Examples:", g_prompt_color, true);
    add_history_line(console, "CURL https://example.com/");
    add_history_line(console, "CURL -L -o PAGE.HTM https://example.com/");
    add_history_line(console, "CURL -X POST -d hello https://example.com/");
}

void run_curl_command(ConsoleState& console, DriveState& drive,
                      const WorkingDirectory& working_directory,
                      const annyos::api::ServiceArguments* arguments) {
    char tokens[annyos::api::kMaximumServiceArguments]
               [annyos::api::kMaximumServiceArgumentBytes + 1]{};
    const uint32_t token_count = arguments == nullptr ? 0 : arguments->count;
    if (token_count > annyos::api::kMaximumServiceArguments) {
        add_history_line(console, "CURL accepts up to 8 arguments. Run CURL -h for help.");
        return;
    }
    for (uint32_t token = 0; token < token_count; ++token) {
        const uint32_t length = bounded_string_length(
            arguments->values[token], annyos::api::kMaximumServiceArgumentBytes + 1);
        if (length > annyos::api::kMaximumServiceArgumentBytes) {
            add_history_line(console, "CURL argument exceeds the AP argument limit.");
            return;
        }
        for (uint32_t index = 0; index <= length; ++index)
            tokens[token][index] = arguments->values[token][index];
    }
    if (token_count == 0 || curl_token_equals(tokens[0], "-h") ||
        curl_token_equals(tokens[0], "--help") || curl_token_equals(tokens[0], "help")) {
        add_curl_help(console);
        return;
    }
    const char* url = nullptr;
    char output_name[13]{};
    bool output_option_seen = false;
    bool output_requested = false;
    bool output_to_console = false;
    bool remote_name = false;
    bool verbose = false;
    bool silent = false;
    for (uint32_t index = 0; index < token_count; ++index) {
        const char* token = tokens[index];
        if (curl_token_equals(token, "-L") || curl_token_equals(token, "--location")) continue;
        if (curl_token_equals(token, "-v") || curl_token_equals(token, "--verbose")) {
            verbose = true;
            continue;
        }
        if (curl_token_equals(token, "-s") || curl_token_equals(token, "--silent")) {
            silent = true;
            continue;
        }
        if (curl_token_equals(token, "-k") || curl_token_equals(token, "--insecure")) continue;
        if (token[0] == '-' && token[1] == 'O' && token[2] == '\0') {
            if (output_option_seen) {
                add_history_line(console, "Use only one of -o <FILE> or -O.");
                return;
            }
            output_option_seen = true;
            output_requested = true;
            remote_name = true;
            continue;
        }

        const bool request_value = (token[0] == '-' && token[1] == 'X' && token[2] == '\0') ||
            curl_token_equals(token, "--request");
        const bool data_value = (token[0] == '-' && token[1] == 'd' && token[2] == '\0') ||
            curl_token_equals(token, "--data");
        const bool header_value = (token[0] == '-' && token[1] == 'H' && token[2] == '\0') ||
            curl_token_equals(token, "--header");
        const bool user_value = (token[0] == '-' && token[1] == 'u' && token[2] == '\0') ||
            curl_token_equals(token, "--user");
        const bool output_value = (token[0] == '-' && token[1] == 'o' && token[2] == '\0') ||
            curl_token_equals(token, "--output");
        if (request_value || data_value || header_value || user_value || output_value) {
            if (index + 1 >= token_count) {
                add_history_line(console, "CURL option is missing its value.");
                return;
            }
            const char* value = tokens[++index];
            if (output_value) {
                if (output_option_seen) {
                    add_history_line(console, "Use only one of -o <FILE> or -O.");
                    return;
                }
                output_option_seen = true;
                output_to_console = curl_token_equals(value, "-");
                output_requested = !output_to_console;
                if (output_requested && !normalize_curl_filename(value, output_name)) {
                    add_history_line(console, "FAT32 output name must use 8.3 format, e.g. PAGE.HTM.");
                    return;
                }
            }
            continue;
        }
        if (token[0] == '-') {
            add_history_line(console, "Unsupported CURL option. Use HELP for supported options.");
            return;
        }
        if (url != nullptr) {
            add_history_line(console, "CURL accepts one URL per command.");
            return;
        }
        url = token;
    }

    if (url == nullptr) {
        add_history_line(console, "Usage: CURL [OPTIONS] URL. Run CURL -h for option details.");
        return;
    }
    if (remote_name) {
        uint32_t end = bounded_string_length(url, kInputCapacity);
        for (uint32_t index = 0; index < end; ++index) {
            if (url[index] == '?' || url[index] == '#') {
                end = index;
                break;
            }
        }
        uint32_t authority_end = 0;
        while (authority_end + 2 < end &&
               !(url[authority_end] == ':' && url[authority_end + 1] == '/' &&
                 url[authority_end + 2] == '/')) {
            ++authority_end;
        }
        if (authority_end + 2 >= end) {
            add_history_line(console, "-O needs a URL with a FAT32 8.3 file name.");
            return;
        }
        uint32_t path_start = authority_end + 3;
        while (path_start < end && url[path_start] != '/') ++path_start;
        if (path_start == end) {
            add_history_line(console, "-O needs a URL with a FAT32 8.3 file name.");
            return;
        }
        uint32_t start = path_start + 1;
        for (uint32_t index = start; index < end; ++index) {
            if (url[index] == '/') start = index + 1;
        }
        if (start >= end || end - start >= sizeof(output_name)) {
            add_history_line(console, "-O needs a URL with a FAT32 8.3 file name.");
            return;
        }
        char remote_filename[13]{};
        for (uint32_t index = start; index < end; ++index) {
            remote_filename[index - start] = url[index];
        }
        if (!normalize_curl_filename(remote_filename, output_name)) {
            add_history_line(console, "URL file name must use FAT32 8.3 format for -O.");
            return;
        }
    }

    const uint32_t url_length = bounded_string_length(url, kInputCapacity);
    const bool http = url_length >= 7 && lowercase(url[0]) == 'h' && lowercase(url[1]) == 't' &&
        lowercase(url[2]) == 't' && lowercase(url[3]) == 'p' && url[4] == ':' &&
        url[5] == '/' && url[6] == '/';
    const bool https = url_length >= 8 && lowercase(url[0]) == 'h' && lowercase(url[1]) == 't' &&
        lowercase(url[2]) == 't' && lowercase(url[3]) == 'p' &&
        lowercase(url[4]) == 's' && url[5] == ':' && url[6] == '/' && url[7] == '/';
    if (!http && !https) {
        add_history_line(console, "Usage: CURL [OPTIONS] http://host/path or https://host/path");
        return;
    }

    char wire_arguments[512]{};
    if (!append_curl_wire_arguments(tokens, token_count, wire_arguments,
                                   sizeof(wire_arguments))) {
        add_history_line(console, "CURL command line is too long.");
        return;
    }
    if (!annyos::curl::fetch_from_host(wire_arguments, &g_curl_response)) {
        add_history_line(console, "Host curl bridge unavailable or request timed out.", g_help_title_color);
        add_history_line(console, "Run in QEMU or configure the VirtualBox COM2 bridge.");
        return;
    }

    if (verbose || (!silent && g_curl_response.curl_exit_code != 0)) {
        print_curl_text(console, g_curl_response.diagnostic,
                        g_curl_response.diagnostic_length);
    }
    if (output_requested && !output_to_console) {
        if (!drive.mounted || working_directory.depth > kWorkingDirectoryDepth) {
            add_history_line(console, "FAT32 drive is not mounted; cannot save the download.",
                             g_help_title_color);
        } else if (!annyos::storage::fat32_write_file(
                       drive.volume,
                       working_directory.cluster_stack[working_directory.depth],
                       output_name, g_curl_response.body, g_curl_response.body_length)) {
            add_history_line(console, "Could not save response to the current FAT32 directory.",
                             g_help_title_color);
        } else {
            char saved_line[64] = "Saved ";
            uint32_t saved_length = 6;
            append_decimal(saved_line, &saved_length, g_curl_response.body_length);
            append_text(saved_line, saved_length, sizeof(saved_line), " bytes to ");
            saved_length = bounded_string_length(saved_line, sizeof(saved_line));
            append_text(saved_line, saved_length, sizeof(saved_line), output_name);
            add_history_line(console, saved_line, g_prompt_color);
        }
    } else if (g_curl_response.body_length != 0) {
        print_curl_text(console, g_curl_response.body, g_curl_response.body_length);
    } else {
        add_history_line(console, "(empty response)");
    }

    if (!silent && g_curl_response.curl_exit_code != 0) {
        char status[48] = "curl exited with code ";
        uint32_t status_length = 22;
        append_decimal(status, &status_length, g_curl_response.curl_exit_code);
        add_history_line(console, status, g_help_title_color);
    }
    if (!silent && g_curl_response.truncated) {
        add_history_line(console, "Response truncated at 8192 bytes.", g_help_title_color);
    }
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
        } else if (annyos::shell::handle_file_command(console, active_drive.volume, active_drive.mounted,
                                               *working_directory, current_theme_colors())) {
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


} // namespace

extern "C" [[noreturn]] void kernel_main(const annyos::BootInfo* boot_info) {
    __asm__ volatile("cli" : : : "memory");
    initialize_kernel_drivers(boot_info);
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

    const auto* ethernet_record =
        g_driver_registry.query(annyos::drivers::ServiceId::ethernet_frames, 1);
    bool network_started = false;
    if (ethernet_record != nullptr) {
        const auto* ethernet = static_cast<const annyos::drivers::service::EthernetFramesV1*>(
            ethernet_record->interface_pointer);
        network_started = annyos::network::network_stack_initialize(*ethernet);
    }
    debug_write(network_started ? "AnnyOS: DHCP discovery sent\r\n"
                                : "AnnyOS: network stack unavailable\r\n");

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
        add_history_line(console, "AnnyOS Command Processor [Version 0.4.8]");
        add_history_line(console, "Type HELP for commands.");
    }
    annyos::shell::ui::redraw_history(boot_info->framebuffer, console, working_directory, current_theme_colors());
    debug_write("AnnyOS: DOS-style command console ready\r\n");
    const annyos::drivers::ServiceRecord* keyboard_record =
        g_driver_registry.query(annyos::drivers::ServiceId::keyboard_input, 1);
    const auto* keyboard = keyboard_record == nullptr ? nullptr :
        static_cast<const annyos::drivers::service::KeyboardInputV1*>(
            keyboard_record->interface_pointer);
    bool dhcp_lease_logged = false;

    for (;;) {
        (void)annyos::network::network_stack_poll(4);
        if (!dhcp_lease_logged && annyos::network::network_interface_config().configured) {
            debug_write("AnnyOS: DHCP IPv4 lease acquired\r\n");
            dhcp_lease_logged = true;
        }
        annyos::input::KeyEvent event{};
        if (keyboard == nullptr || !keyboard->poll(&event)) {
            __asm__ volatile("pause");
            continue;
        }

        if (console.editor_active) {
            annyos::shell::handle_editor_event(console,
                g_drives[working_directory.drive_index].volume, working_directory, event,
                current_theme_colors());
            if (console.editor_active) {
                annyos::shell::ui::draw_editor(boot_info->framebuffer, console, current_theme_colors());
            } else {
                annyos::shell::ui::redraw_history(boot_info->framebuffer, console, working_directory, current_theme_colors());
            }
            continue;
        }

        if (event.action == annyos::input::KeyAction::enter) {
            submit_command(console, g_drives, &working_directory, api);
            annyos::shell::ui::redraw_history(boot_info->framebuffer, console, working_directory, current_theme_colors());
        } else if (event.action == annyos::input::KeyAction::up ||
                   event.action == annyos::input::KeyAction::down) {
            recall_command(console, event.action == annyos::input::KeyAction::up);
            annyos::shell::ui::redraw_prompt(boot_info->framebuffer, console, working_directory, current_theme_colors());
        } else if (event.action == annyos::input::KeyAction::backspace) {
            console.command_history_cursor = console.command_history_count;
            if (console.input_length > 0) console.input[--console.input_length] = '\0';
            annyos::shell::ui::redraw_prompt(boot_info->framebuffer, console, working_directory, current_theme_colors());
        } else if (event.action == annyos::input::KeyAction::character &&
                   console.input_length + 1 < kInputCapacity && event.character >= 32 && event.character <= 126) {
            console.command_history_cursor = console.command_history_count;
            const int32_t available_width = static_cast<int32_t>(boot_info->framebuffer.width) - 2 * kMargin;
            const int32_t required_width = annyos::shell::ui::text_width(working_directory.path) + annyos::shell::ui::text_width("> ") +
                                           annyos::shell::ui::text_width(console.input) + annyos::shell::ui::glyph_advance(event.character);
            if (required_width < available_width) {
                console.input[console.input_length++] = event.character;
                console.input[console.input_length] = '\0';
            }
            annyos::shell::ui::redraw_prompt(boot_info->framebuffer, console, working_directory, current_theme_colors());
        }
    }
}
