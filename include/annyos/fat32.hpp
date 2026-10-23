#pragma once

#include "annyos/block_device.hpp"

namespace annyos::storage {

struct Fat32Volume {
    BlockDevice device;
    uint64_t volume_start_lba;
    uint64_t volume_sector_count;
    uint32_t fat_start_lba;
    uint32_t data_start_lba;
    uint32_t sectors_per_fat;
    uint32_t root_cluster;
    uint32_t cluster_count;
    uint8_t sectors_per_cluster;
    uint8_t fat_count;
};

struct Fat32DirectoryEntry {
    char name[13];
    uint32_t size;
    uint32_t first_cluster;
    bool is_directory;
};

using Fat32DirectoryCallback = bool (*)(void* context, const Fat32DirectoryEntry& entry);

// Mounts a FAT32 volume at the start of the supplied block device. This
// initial driver supports sector reads and root-directory enumeration.
bool fat32_mount(const BlockDevice& device, Fat32Volume* volume);
bool fat32_list_root(const Fat32Volume& volume, Fat32DirectoryCallback callback, void* context);
bool fat32_list_directory(const Fat32Volume& volume, uint32_t directory_cluster,
                          Fat32DirectoryCallback callback, void* context);
bool fat32_find_file(const Fat32Volume& volume, uint32_t directory_cluster,
                     const char* short_name, Fat32DirectoryEntry* entry);
bool fat32_read_file(const Fat32Volume& volume, const Fat32DirectoryEntry& entry,
                     uint8_t* output, uint32_t output_capacity, uint32_t* bytes_read);
// Creates or replaces a short-name file with data occupying at most one FAT32
// cluster. The operation writes a fresh cluster before publishing its directory
// entry, so an interrupted update leaves the previous file contents intact.
bool fat32_write_file(const Fat32Volume& volume, uint32_t directory_cluster,
                      const char* short_name, const uint8_t* data, uint32_t size);

} // namespace annyos::storage
