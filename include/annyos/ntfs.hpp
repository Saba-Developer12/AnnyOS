#pragma once

#include "annyos/block_device.hpp"

namespace annyos::storage {

// Validated NTFS boot-sector metadata. This probe does not mount the MFT or
// expose file operations; callers must not treat recognition as read support.
struct NtfsVolumeInfo {
    uint64_t partition_start_lba;
    uint64_t sector_count;
    uint64_t mft_cluster;
    uint64_t mft_mirror_cluster;
    uint64_t serial_number;
    uint32_t bytes_per_sector;
    uint32_t cluster_size_bytes;
    uint32_t file_record_size_bytes;
    uint8_t sectors_per_cluster;
};

bool ntfs_probe(const BlockDevice& device, NtfsVolumeInfo* volume);

} // namespace annyos::storage
