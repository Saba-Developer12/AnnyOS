#include "annyos/apfs.hpp"

extern "C" bool annyos_apfs_probe(const annyos::storage::BlockDevice* device,
                                  annyos::storage::ApfsContainerInfo* container) {
    return device != nullptr && annyos::storage::apfs_probe(*device, container);
}
