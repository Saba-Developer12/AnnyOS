#pragma once

#include <stdint.h>

namespace annyos {

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
    const void* memory_map;
    uint64_t memory_map_size;
    uint64_t memory_descriptor_size;
    uint32_t memory_descriptor_version;
};

} // namespace annyos
