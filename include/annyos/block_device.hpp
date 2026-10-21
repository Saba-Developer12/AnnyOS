#pragma once

#include <stdint.h>

namespace annyos::storage {

// A sector device is supplied by a hardware driver. Filesystem code never
// depends on a particular ATA, AHCI, USB, or firmware implementation.
struct BlockDevice {
    void* context;
    uint64_t sector_count;
    uint32_t sector_size;
    bool (*read_sector)(void* context, uint64_t lba, uint8_t* destination);
};

} // namespace annyos::storage
