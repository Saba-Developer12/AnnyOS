#pragma once

#include <stdint.h>

namespace annyos {

// UEFI x64 uses the Microsoft ABI even though the kernel is built as
// freestanding C++. Keep the runtime reset entry point across ExitBootServices
// so the kernel can request a firmware-managed platform reset.
using UefiResetSystem = void (__attribute__((ms_abi)) *)(
    uint32_t reset_type, uint64_t reset_status, uintptr_t data_size, void* reset_data);

enum class PixelFormat : uint32_t {
    rgb_reserved_8 = 0,
    bgr_reserved_8 = 1,
    bit_mask = 2,
    blt_only = 3,
};

struct PixelBitmask {
    uint32_t red;
    uint32_t green;
    uint32_t blue;
    uint32_t reserved;
};

struct FramebufferInfo {
    volatile uint32_t* address;
    uint64_t size_bytes;
    uint32_t width;
    uint32_t height;
    uint32_t pixels_per_scanline;
    PixelFormat format;
    PixelBitmask mask;
};

struct MemoryDescriptor {
    uint32_t type;
    uint32_t padding;
    uint64_t physical_start;
    uint64_t virtual_start;
    uint64_t number_of_pages;
    uint64_t attributes;
};

// The pointed-to descriptors are the raw UEFI memory map. Their stride is
// descriptor_size bytes, as specified by UEFI, and may exceed sizeof a local
// descriptor structure on a different firmware implementation.
struct BootInfo {
    FramebufferInfo framebuffer;
    // UEFI-owned, physically contiguous memory reserved for device DMA. The
    // allocator constrains this region below 4 GiB for legacy PCI devices.
    void* dma_region;
    uint64_t dma_physical_address;
    uint64_t dma_region_size;
    const void* memory_map;
    uint64_t memory_map_size;
    uint64_t memory_descriptor_size;
    uint32_t memory_descriptor_version;
    UefiResetSystem reset_system;
};

} // namespace annyos
