#pragma once

#include <stdint.h>

namespace annyos::api {

constexpr uint32_t kVersion = 3;
constexpr uint32_t kMaximumServiceArguments = 8;
constexpr uint32_t kMaximumServiceArgumentBytes = 95;

enum class Service : uint8_t {
    version = 1,
    date = 2,
    time = 3,
    memory = 4,
    reboot = 5,
    sleep = 6,
    beep = 7,
    config_list = 8,
    config_get_theme = 9,
    config_set_theme_blue = 10,
    config_set_theme_green = 11,
    config_set_theme_amber = 12,
    config_set_theme_matrix = 13,
    sys_shutdown = 14,
    sys_uptime = 15,
    sys_clear = 16,
    sys_get_tick = 17,
    sys_set_env = 18,
    sys_get_env = 19,
    sys_cpu_info = 20,
    sys_panic = 21,
    fs_create = 22,
    fs_read = 23,
    fs_edit = 24,
    fs_delete = 25,
    fs_exists = 26,
    fs_list_dir = 27,
    fs_mkdir = 28,
    fs_rmdir = 29,
    fs_copy = 30,
    fs_move = 31,
    fs_mount_apfs = 32,
    fs_mount_ntfs = 33,
    mem_alloc = 34,
    mem_free = 35,
    mem_total = 36,
    mem_free_space = 37,
    mem_copy = 38,
    mem_set = 39,
    disk_info = 40,
    disk_format = 41,
    play_tone = 42,
    audio_stop = 43,
    sound_effect = 44,
    speaker_init = 45,
    audio_volume = 46,
    ui_set_title = 47,
    ui_set_color = 48,
    ui_draw_pixel = 49,
    ui_draw_rect = 50,
    ui_clear_screen = 51,
    ui_print_text = 52,
    ui_cursor_pos = 53,
    ui_open_editor = 54,
    ui_theme_dark = 55,
    ui_theme_light = 56,
    inp_read_key = 57,
    inp_check_key = 58,
    mouse_get_pos = 59,
    mouse_check_click = 60,
    serial_write = 61,
    serial_read = 62,
    usb_scan = 63,
    time_get_rtc = 64,

    // Public AADL/C++ spellings. SYS_VERSION, SYS_REBOOT, and BEEP retain the
    // original v1 service numbers so existing AP binaries remain compatible.
    SYS_VERSION = version,
    SYS_REBOOT = reboot,
    SYS_SHUTDOWN = sys_shutdown,
    SYS_UPTIME = sys_uptime,
    SYS_CLEAR = sys_clear,
    SYS_GET_TICK = sys_get_tick,
    SYS_SET_ENV = sys_set_env,
    SYS_GET_ENV = sys_get_env,
    SYS_CPU_INFO = sys_cpu_info,
    SYS_PANIC = sys_panic,
    FS_CREATE = fs_create,
    FS_READ = fs_read,
    FS_EDIT = fs_edit,
    FS_DELETE = fs_delete,
    FS_EXISTS = fs_exists,
    FS_LIST_DIR = fs_list_dir,
    FS_MKDIR = fs_mkdir,
    FS_RMDIR = fs_rmdir,
    FS_COPY = fs_copy,
    FS_MOVE = fs_move,
    FS_MOUNT_APFS = fs_mount_apfs,
    FS_MOUNT_NTFS = fs_mount_ntfs,
    MEM_ALLOC = mem_alloc,
    MEM_FREE = mem_free,
    MEM_TOTAL = mem_total,
    MEM_FREE_SPACE = mem_free_space,
    MEM_COPY = mem_copy,
    MEM_SET = mem_set,
    DISK_INFO = disk_info,
    DISK_FORMAT = disk_format,
    BEEP = beep,
    PLAY_TONE = play_tone,
    AUDIO_STOP = audio_stop,
    SOUND_EFFECT = sound_effect,
    SPEAKER_INIT = speaker_init,
    AUDIO_VOLUME = audio_volume,
    UI_SET_TITLE = ui_set_title,
    UI_SET_COLOR = ui_set_color,
    UI_DRAW_PIXEL = ui_draw_pixel,
    UI_DRAW_RECT = ui_draw_rect,
    UI_CLEAR_SCREEN = ui_clear_screen,
    UI_PRINT_TEXT = ui_print_text,
    UI_CURSOR_POS = ui_cursor_pos,
    UI_OPEN_EDITOR = ui_open_editor,
    UI_THEME_DARK = ui_theme_dark,
    UI_THEME_LIGHT = ui_theme_light,
    INP_READ_KEY = inp_read_key,
    INP_CHECK_KEY = inp_check_key,
    MOUSE_GET_POS = mouse_get_pos,
    MOUSE_CHECK_CLICK = mouse_check_click,
    SERIAL_WRITE = serial_write,
    SERIAL_READ = serial_read,
    USB_SCAN = usb_scan,
    TIME_GET_RTC = time_get_rtc,
};

struct ServiceArguments {
    uint8_t count;
    char values[kMaximumServiceArguments][kMaximumServiceArgumentBytes + 1];
};

using ConsoleWriteLine = bool (*)(void* context, const char* text, uint8_t length);
using ConsoleWriteTitle = bool (*)(void* context, const char* text, uint8_t length);
using ConsoleWriteHelpRow = bool (*)(void* context, const char* command, uint8_t command_length,
                                     const char* description, uint8_t description_length);
using InvokeService = bool (*)(void* context, Service service);
using InvokeServiceV3 = bool (*)(void* context, Service service,
                                 const ServiceArguments* arguments);

// Stable host table presented to .AP bytecode applications. API v2 extends
// the service identifiers while retaining this v1 table layout.
struct AnnyOsApiV1 {
    // Hosts may append fields in later versions; the V1 prefix remains stable.
    uint32_t structure_size;
    uint32_t version;
    void* context;
    ConsoleWriteLine console_write_line;
    ConsoleWriteTitle console_write_title;
    ConsoleWriteHelpRow console_write_help_row;
    InvokeService invoke_service;
    // API v3 appends an argument-aware dispatch while preserving the v1/v2
    // prefix above. Bytecode services receive the application's bounded argv.
    InvokeServiceV3 invoke_service_v3;
};

} // namespace annyos::api
