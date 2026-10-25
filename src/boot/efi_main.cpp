#include "uefi.hpp"

namespace annyos::boot {
bool find_framebuffer(EFI_BOOT_SERVICES*, annyos::FramebufferInfo*);
void debug_write(const char*);
}

extern "C" [[noreturn]] void kernel_main(const annyos::BootInfo* boot_info);

namespace {
constexpr EFI_UINTN kMemoryMapCapacity = 1024 * 1024;

void console_write(EFI_SYSTEM_TABLE* system_table, const EFI_CHAR16* text) {
    if (system_table != nullptr && system_table->console_out != nullptr &&
        system_table->console_out->output_string != nullptr) {
        system_table->console_out->output_string(system_table->console_out, text);
    }
}

[[noreturn]] void halt() {
    for (;;) __asm__ volatile("hlt");
}
}

extern "C" EFI_STATUS EFIAPI efi_main(EFI_HANDLE image_handle, EFI_SYSTEM_TABLE* system_table) {
    if (system_table == nullptr || system_table->boot_services == nullptr) return EFI_INVALID_PARAMETER;
    EFI_BOOT_SERVICES* services = system_table->boot_services;
    console_write(system_table, u"AnnyOS UEFI loader\r\n");
    annyos::boot::debug_write("AnnyOS: UEFI loader started\r\n");

    annyos::BootInfo boot_info{};
    if (system_table->runtime_services != nullptr) {
        boot_info.reset_system = system_table->runtime_services->reset_system;
    }
    if (!annyos::boot::find_framebuffer(services, &boot_info.framebuffer)) {
        console_write(system_table, u"AnnyOS: GOP framebuffer unavailable\r\n");
        annyos::boot::debug_write("AnnyOS: ERROR GOP framebuffer unavailable\r\n");
        halt();
    }

    void* memory_map = nullptr;
    EFI_STATUS status = services->allocate_pool(EFI_LOADER_DATA, kMemoryMapCapacity, &memory_map);
    if (status != EFI_SUCCESS || memory_map == nullptr) {
        console_write(system_table, u"AnnyOS: memory map allocation failed\r\n");
        annyos::boot::debug_write("AnnyOS: ERROR memory map allocation failed\r\n");
        halt();
    }

    EFI_UINTN map_size = kMemoryMapCapacity;
    EFI_UINTN map_key = 0;
    EFI_UINTN descriptor_size = 0;
    uint32_t descriptor_version = 0;
    status = services->get_memory_map(&map_size, memory_map, &map_key,
                                      &descriptor_size, &descriptor_version);
    if (status != EFI_SUCCESS) {
        console_write(system_table, u"AnnyOS: unable to read UEFI memory map\r\n");
        annyos::boot::debug_write("AnnyOS: ERROR unable to read UEFI memory map\r\n");
        halt();
    }

    boot_info.memory_map = memory_map;
    boot_info.memory_map_size = map_size;
    boot_info.memory_descriptor_size = descriptor_size;
    boot_info.memory_descriptor_version = descriptor_version;

    status = services->exit_boot_services(image_handle, map_key);
    if (status == EFI_INVALID_PARAMETER) {
        map_size = kMemoryMapCapacity;
        status = services->get_memory_map(&map_size, memory_map, &map_key,
                                          &descriptor_size, &descriptor_version);
        if (status == EFI_SUCCESS) {
            boot_info.memory_map_size = map_size;
            boot_info.memory_descriptor_size = descriptor_size;
            boot_info.memory_descriptor_version = descriptor_version;
            status = services->exit_boot_services(image_handle, map_key);
        }
    }
    if (status != EFI_SUCCESS) {
        annyos::boot::debug_write("AnnyOS: ERROR ExitBootServices failed\r\n");
        halt();
    }

    annyos::boot::debug_write("AnnyOS: ExitBootServices complete\r\n");
    annyos::boot::debug_write("AnnyOS: kernel handoff\r\n");
    kernel_main(&boot_info);
}
