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

// Mounts a FAT32 volume found on the supplied block device.
bool fat32_mount(const BlockDevice& device, Fat32Volume* volume);
bool fat32_list_root(const Fat32Volume& volume, Fat32DirectoryCallback callback, void* context);
bool fat32_list_directory(const Fat32Volume& volume, uint32_t directory_cluster,
                          Fat32DirectoryCallback callback, void* context);
bool fat32_find_file(const Fat32Volume& volume, uint32_t directory_cluster,
                     const char* short_name, Fat32DirectoryEntry* entry);
bool fat32_read_file(const Fat32Volume& volume, const Fat32DirectoryEntry& entry,
                     uint8_t* output, uint32_t output_capacity, uint32_t* bytes_read);
// Creates or atomically replaces a short-name file. Data may span multiple
// clusters; a zero-sized file has no cluster chain.
bool fat32_write_file(const Fat32Volume& volume, uint32_t directory_cluster,
                      const char* short_name, const uint8_t* data, uint32_t size);
// Removes a file entry and releases its FAT cluster chain. Directories are not
// removed by this operation.
bool fat32_delete_file(const Fat32Volume& volume, uint32_t directory_cluster,
                       const char* short_name);
bool fat32_make_directory(const Fat32Volume& volume, uint32_t parent_cluster,
                          const char* short_name);
bool fat32_remove_directory(const Fat32Volume& volume, uint32_t parent_cluster,
                            const char* short_name);
bool fat32_copy_file(const Fat32Volume& volume, uint32_t directory_cluster,
                     const char* source_name, const char* destination_name);
bool fat32_move_file(const Fat32Volume& volume, uint32_t directory_cluster,
                     const char* source_name, const char* destination_name);
bool fat32_count_free_clusters(const Fat32Volume& volume, uint32_t* free_clusters);

} // namespace annyos::storage
