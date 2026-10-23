#pragma once

#include "annyos/block_device.hpp"

namespace annyos::storage {

struct AtaPioDevice {
    uint16_t io_base;
    uint16_t control_port;
    uint8_t drive;
    uint32_t sector_count;
};

// Initializes one legacy ATA channel (0=primary, 1=secondary) and drive
// (0=master, 1=slave), then exposes it through the common block-device API.
bool ata_pio_initialize(uint8_t channel, uint8_t drive, AtaPioDevice* state, BlockDevice* device);
bool ata_pio_read_sector(void* context, uint64_t lba, uint8_t* destination);
bool ata_pio_write_sector(void* context, uint64_t lba, const uint8_t* source);

} // namespace annyos::storage
