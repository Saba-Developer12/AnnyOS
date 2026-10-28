#pragma once

#include <stdint.h>
#include <stddef.h>

#include "annyos/boot_info.hpp"

#define EFIAPI __attribute__((ms_abi))

using EFI_STATUS = uint64_t;
using EFI_HANDLE = void*;
using EFI_EVENT = void*;
using EFI_UINTN = uintptr_t;
using EFI_CHAR16 = char16_t;

constexpr EFI_STATUS EFI_SUCCESS = 0;
constexpr EFI_STATUS EFI_ERROR_BIT = 1ull << 63;
constexpr EFI_STATUS EFI_BUFFER_TOO_SMALL = EFI_ERROR_BIT | 5;
constexpr EFI_STATUS EFI_INVALID_PARAMETER = EFI_ERROR_BIT | 2;
constexpr uint32_t EFI_LOADER_DATA = 2;

struct EFI_TABLE_HEADER {
    uint64_t signature;
    uint32_t revision;
    uint32_t header_size;
    uint32_t crc32;
    uint32_t reserved;
};

struct EFI_GUID {
    uint32_t data1;
    uint16_t data2;
    uint16_t data3;
    uint8_t data4[8];
};

struct EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL {
    void* reset;
    EFI_STATUS (EFIAPI *output_string)(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL*, const EFI_CHAR16*);
};

struct EFI_PIXEL_BITMASK {
    uint32_t red;
    uint32_t green;
    uint32_t blue;
    uint32_t reserved;
};

struct EFI_GRAPHICS_OUTPUT_MODE_INFORMATION {
    uint32_t version;
    uint32_t horizontal_resolution;
    uint32_t vertical_resolution;
    uint32_t pixel_format;
    EFI_PIXEL_BITMASK pixel_information;
    uint32_t pixels_per_scanline;
};

struct EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE {
    uint32_t max_mode;
    uint32_t mode;
    EFI_GRAPHICS_OUTPUT_MODE_INFORMATION* info;
    EFI_UINTN size_of_info;
    uint64_t framebuffer_base;
    uint64_t framebuffer_size;
};

struct EFI_GRAPHICS_OUTPUT_PROTOCOL {
    void* query_mode;
    void* set_mode;
    void* blt;
    EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE* mode;
};

using EFI_GET_MEMORY_MAP = EFI_STATUS (EFIAPI *)(EFI_UINTN*, void*, EFI_UINTN*, EFI_UINTN*, uint32_t*);
using EFI_ALLOCATE_POOL = EFI_STATUS (EFIAPI *)(uint32_t, EFI_UINTN, void**);
using EFI_ALLOCATE_PAGES = EFI_STATUS (EFIAPI *)(uint32_t, uint32_t, EFI_UINTN, uint64_t*);
using EFI_EXIT_BOOT_SERVICES = EFI_STATUS (EFIAPI *)(EFI_HANDLE, EFI_UINTN);
using EFI_LOCATE_PROTOCOL = EFI_STATUS (EFIAPI *)(EFI_GUID*, void*, void**);
using EFI_RESET_SYSTEM = void (EFIAPI *)(uint32_t, EFI_STATUS, EFI_UINTN, void*);

struct EFI_RUNTIME_SERVICES {
    EFI_TABLE_HEADER header;
    void* get_time;
    void* set_time;
    void* get_wakeup_time;
    void* set_wakeup_time;
    void* set_virtual_address_map;
    void* convert_pointer;
    void* get_variable;
    void* get_next_variable_name;
    void* set_variable;
    void* get_next_high_monotonic_count;
    EFI_RESET_SYSTEM reset_system;
};
static_assert(offsetof(EFI_RUNTIME_SERVICES, reset_system) == 104,
              "UEFI runtime service table layout changed");

struct EFI_BOOT_SERVICES {
    EFI_TABLE_HEADER header;
    void* raise_tpl;
    void* restore_tpl;
    EFI_ALLOCATE_PAGES allocate_pages;
    void* free_pages;
    EFI_GET_MEMORY_MAP get_memory_map;
    EFI_ALLOCATE_POOL allocate_pool;
    void* free_pool;
    void* create_event;
    void* set_timer;
    void* wait_for_event;
    void* signal_event;
    void* close_event;
    void* check_event;
    void* install_protocol_interface;
    void* reinstall_protocol_interface;
    void* uninstall_protocol_interface;
    void* handle_protocol;
    void* reserved;
    void* register_protocol_notify;
    void* locate_handle;
    void* locate_device_path;
    void* install_configuration_table;
    void* load_image;
    void* start_image;
    void* exit;
    void* unload_image;
    EFI_EXIT_BOOT_SERVICES exit_boot_services;
    void* get_next_monotonic_count;
    void* stall;
    void* set_watchdog_timer;
    void* connect_controller;
    void* disconnect_controller;
    void* open_protocol;
    void* close_protocol;
    void* open_protocol_information;
    void* protocols_per_handle;
    void* locate_handle_buffer;
    EFI_LOCATE_PROTOCOL locate_protocol;
};

struct EFI_SYSTEM_TABLE {
    EFI_TABLE_HEADER header;
    EFI_CHAR16* firmware_vendor;
    uint32_t firmware_revision;
    EFI_HANDLE console_in_handle;
    void* console_in;
    EFI_HANDLE console_out_handle;
    EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL* console_out;
    EFI_HANDLE standard_error_handle;
    void* standard_error;
    EFI_RUNTIME_SERVICES* runtime_services;
    EFI_BOOT_SERVICES* boot_services;
};

struct UEFI_MEMORY_DESCRIPTOR {
    uint32_t type;
    uint32_t padding;
    uint64_t physical_start;
    uint64_t virtual_start;
    uint64_t number_of_pages;
    uint64_t attribute;
};

namespace annyos::boot {

bool find_framebuffer(EFI_BOOT_SERVICES* services, annyos::FramebufferInfo* out);
void debug_write(const char* text);

} // namespace annyos::boot
