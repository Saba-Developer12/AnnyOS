#pragma once

#include "annyos/block_device.hpp"

namespace annyos::storage {

// Validated APFS container-superblock metadata. This is a container probe,
// not a mounted APFS volume; encryption, checkpoints and B-trees are not yet
// interpreted by the kernel.
struct ApfsContainerInfo {
    uint64_t partition_start_lba;
    uint64_t partition_sector_count;
    uint64_t block_count;
    uint64_t next_transaction_id;
    uint32_t block_size;
    uint32_t max_file_systems;
};

bool apfs_probe(const BlockDevice& device, ApfsContainerInfo* container);

} // namespace annyos::storage
