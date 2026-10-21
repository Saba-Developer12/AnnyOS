#include "annyos/fat32.hpp"

extern "C" bool annyos_fat32_mount(const annyos::storage::BlockDevice* device,
                                   annyos::storage::Fat32Volume* volume) {
    return device != nullptr && annyos::storage::fat32_mount(*device, volume);
}

extern "C" bool annyos_fat32_list_root(const annyos::storage::Fat32Volume* volume,
                                       annyos::storage::Fat32DirectoryCallback callback,
                                       void* context) {
    return volume != nullptr && annyos::storage::fat32_list_root(*volume, callback, context);
}

extern "C" bool annyos_fat32_list_directory(const annyos::storage::Fat32Volume* volume,
                                            uint32_t cluster,
                                            annyos::storage::Fat32DirectoryCallback callback,
                                            void* context) {
    return volume != nullptr && annyos::storage::fat32_list_directory(*volume, cluster, callback, context);
}

extern "C" bool annyos_fat32_find_file(const annyos::storage::Fat32Volume* volume,
                                       uint32_t cluster, const char* short_name,
                                       annyos::storage::Fat32DirectoryEntry* entry) {
    return volume != nullptr &&
           annyos::storage::fat32_find_file(*volume, cluster, short_name, entry);
}

extern "C" bool annyos_fat32_read_file(const annyos::storage::Fat32Volume* volume,
                                       const annyos::storage::Fat32DirectoryEntry* entry,
                                       uint8_t* output, uint32_t capacity, uint32_t* bytes_read) {
    return volume != nullptr && entry != nullptr &&
           annyos::storage::fat32_read_file(*volume, *entry, output, capacity, bytes_read);
}
