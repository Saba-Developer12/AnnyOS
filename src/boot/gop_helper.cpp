#include "uefi.hpp"

namespace annyos::boot {

namespace {
constexpr EFI_GUID kGraphicsOutputProtocolGuid = {
    0x9042a9de, 0x23dc, 0x4a38,
    {0x96, 0xfb, 0x7a, 0xde, 0xd0, 0x80, 0x51, 0x6a},
};
}

bool find_framebuffer(EFI_BOOT_SERVICES* services, annyos::FramebufferInfo* out) {
    if (services == nullptr || out == nullptr || services->locate_protocol == nullptr) {
        return false;
    }

    EFI_GRAPHICS_OUTPUT_PROTOCOL* gop = nullptr;
    const EFI_STATUS status = services->locate_protocol(
        const_cast<EFI_GUID*>(&kGraphicsOutputProtocolGuid), nullptr,
        reinterpret_cast<void**>(&gop));
    if (status != EFI_SUCCESS || gop == nullptr || gop->mode == nullptr || gop->mode->info == nullptr) {
        return false;
    }

    const auto* info = gop->mode->info;
    if (gop->mode->framebuffer_base == 0 || info->horizontal_resolution == 0 ||
        info->vertical_resolution == 0 || info->pixels_per_scanline < info->horizontal_resolution ||
        info->pixel_format > 2) {
        return false;
    }
    if (info->pixel_format == 2 &&
        (info->pixel_information.red == 0 || info->pixel_information.green == 0 ||
         info->pixel_information.blue == 0)) {
        return false;
    }
    const uint64_t framebuffer_bytes_required =
        static_cast<uint64_t>(info->pixels_per_scanline) * info->vertical_resolution * sizeof(uint32_t);
    if (gop->mode->framebuffer_size < framebuffer_bytes_required) return false;

    out->address = reinterpret_cast<volatile uint32_t*>(gop->mode->framebuffer_base);
    out->size_bytes = gop->mode->framebuffer_size;
    out->width = info->horizontal_resolution;
    out->height = info->vertical_resolution;
    out->pixels_per_scanline = info->pixels_per_scanline;
    out->format = static_cast<annyos::PixelFormat>(info->pixel_format);
    out->mask = {info->pixel_information.red, info->pixel_information.green,
                 info->pixel_information.blue, info->pixel_information.reserved};
    return true;
}

void debug_write(const char* text) {
    if (text == nullptr) return;
    while (*text != '\0') {
        const uint8_t byte = static_cast<uint8_t>(*text++);
        __asm__ volatile("outb %0, $0xe9" : : "a"(byte));
    }
}

} // namespace annyos::boot
