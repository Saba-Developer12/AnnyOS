#include "annyos/ntfs.hpp"

extern "C" bool annyos_ntfs_probe(const annyos::storage::BlockDevice* device,
                                  annyos::storage::NtfsVolumeInfo* volume) {
    return device != nullptr && annyos::storage::ntfs_probe(*device, volume);
}
