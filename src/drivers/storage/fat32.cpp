#include "annyos/fat32.hpp"

namespace annyos::storage {
namespace {
constexpr uint32_t kSectorBytes = 512;
constexpr uint32_t kFatEndOfChain = 0x0ffffff8;
constexpr uint32_t kFatBadCluster = 0x0ffffff7;
constexpr uint8_t kEspTypeGuid[16] = {
    0x28, 0x73, 0x2a, 0xc1, 0x1f, 0xf8, 0xd2, 0x11,
    0xba, 0x4b, 0x00, 0xa0, 0xc9, 0x3e, 0xc9, 0x3b,
};

uint16_t read_u16(const uint8_t* bytes) {
    return static_cast<uint16_t>(bytes[0]) | (static_cast<uint16_t>(bytes[1]) << 8);
}

uint32_t read_u32(const uint8_t* bytes) {
    return static_cast<uint32_t>(bytes[0]) |
           (static_cast<uint32_t>(bytes[1]) << 8) |
           (static_cast<uint32_t>(bytes[2]) << 16) |
           (static_cast<uint32_t>(bytes[3]) << 24);
}

bool cluster_lba(const Fat32Volume& volume, uint32_t cluster, uint32_t sector_in_cluster,
                 uint32_t* lba) {
    if (cluster < 2 || cluster >= volume.cluster_count + 2 ||
        sector_in_cluster >= volume.sectors_per_cluster) return false;
    const uint64_t address = volume.volume_start_lba + volume.data_start_lba +
        static_cast<uint64_t>(cluster - 2) * volume.sectors_per_cluster + sector_in_cluster;
    if (address >= volume.device.sector_count || address > 0xffffffffu) return false;
    *lba = static_cast<uint32_t>(address);
    return true;
}

bool next_cluster(const Fat32Volume& volume, uint32_t cluster, uint8_t* sector, uint32_t* next) {
    const uint32_t offset = cluster * 4;
    const uint32_t fat_sector = offset / kSectorBytes;
    if (fat_sector >= volume.sectors_per_fat) return false;
    const uint64_t lba = volume.volume_start_lba + volume.fat_start_lba + fat_sector;
    if (lba >= volume.device.sector_count ||
        !volume.device.read_sector(volume.device.context, lba, sector)) return false;
    *next = read_u32(sector + (offset % kSectorBytes)) & 0x0fffffffu;
    return true;
}

bool looks_like_fat32(const uint8_t* sector) {
    return sector[510] == 0x55 && sector[511] == 0xaa &&
           read_u16(sector + 11) == kSectorBytes && read_u16(sector + 17) == 0 &&
           read_u32(sector + 36) != 0;
}

bool find_volume_start(const BlockDevice& device, uint8_t* sector, uint64_t* start_lba,
                       uint64_t* available_sectors) {
    if (!device.read_sector(device.context, 0, sector)) return false;
    if (looks_like_fat32(sector)) {
        *start_lba = 0;
        *available_sectors = device.sector_count;
        return true;
    }
    if (sector[510] != 0x55 || sector[511] != 0xaa) return false;

    // Support conventional MBR FAT32 partitions as well as GPT EFI System
    // Partitions. This disk image uses GPT and starts its ESP at LBA 2048.
    for (uint32_t entry = 0; entry < 4; ++entry) {
        const uint8_t* partition = sector + 446 + entry * 16;
        const uint8_t type = partition[4];
        const uint32_t first = read_u32(partition + 8);
        const uint32_t count = read_u32(partition + 12);
        if ((type == 0x0b || type == 0x0c || type == 0xef) && count != 0 &&
            static_cast<uint64_t>(first) + count <= device.sector_count) {
            *start_lba = first;
            *available_sectors = count;
            return true;
        }
    }

    if (!device.read_sector(device.context, 1, sector) ||
        sector[0] != 'E' || sector[1] != 'F' || sector[2] != 'I' || sector[3] != ' ' ||
        sector[4] != 'P' || sector[5] != 'A' || sector[6] != 'R' || sector[7] != 'T') return false;
    const uint64_t entries_lba = static_cast<uint64_t>(read_u32(sector + 72)) |
                                 (static_cast<uint64_t>(read_u32(sector + 76)) << 32);
    const uint32_t entry_count = read_u32(sector + 80);
    const uint32_t entry_size = read_u32(sector + 84);
    if (entries_lba >= device.sector_count || entry_count == 0 || entry_size < 128 ||
        entry_size > kSectorBytes || (entry_size % 8) != 0 || (kSectorBytes % entry_size) != 0) return false;

    uint8_t entries_sector[kSectorBytes];
    uint32_t cached_sector = 0xffffffffu;
    for (uint32_t index = 0; index < entry_count; ++index) {
        const uint64_t byte_offset = static_cast<uint64_t>(index) * entry_size;
        const uint64_t sector_index = byte_offset / kSectorBytes;
        if (sector_index > 0xffffffffu || entries_lba + sector_index >= device.sector_count) return false;
        if (cached_sector != sector_index) {
            if (!device.read_sector(device.context, entries_lba + sector_index, entries_sector)) return false;
            cached_sector = static_cast<uint32_t>(sector_index);
        }
        const uint8_t* partition = entries_sector + (byte_offset % kSectorBytes);
        bool is_esp = true;
        for (uint32_t byte = 0; byte < sizeof(kEspTypeGuid); ++byte) {
            if (partition[byte] != kEspTypeGuid[byte]) is_esp = false;
        }
        if (!is_esp) continue;
        const uint64_t first = static_cast<uint64_t>(read_u32(partition + 32)) |
                               (static_cast<uint64_t>(read_u32(partition + 36)) << 32);
        const uint64_t last = static_cast<uint64_t>(read_u32(partition + 40)) |
                              (static_cast<uint64_t>(read_u32(partition + 44)) << 32);
        if (last < first || last >= device.sector_count) return false;
        *start_lba = first;
        *available_sectors = last - first + 1;
        return true;
    }
    return false;
}

char uppercase(char value) {
    return value >= 'a' && value <= 'z' ? static_cast<char>(value - 'a' + 'A') : value;
}

void decode_short_name(const uint8_t* raw, char* output) {
    uint32_t at = 0;
    for (uint32_t i = 0; i < 8 && raw[i] != ' '; ++i) output[at++] = uppercase(static_cast<char>(raw[i]));
    if (raw[8] != ' ') {
        output[at++] = '.';
        for (uint32_t i = 8; i < 11 && raw[i] != ' '; ++i) output[at++] = uppercase(static_cast<char>(raw[i]));
    }
    output[at] = '\0';
}

struct DirectorySlot {
    uint32_t lba;
    uint16_t offset;
    uint32_t old_first_cluster;
    bool existed;
};

bool encode_short_name(const char* name, uint8_t output[11]) {
    if (name == nullptr) return false;
    for (uint32_t i = 0; i < 11; ++i) output[i] = ' ';
    uint32_t base = 0;
    uint32_t extension = 0;
    bool after_dot = false;
    for (const char* cursor = name; *cursor != '\0'; ++cursor) {
        if (*cursor == '.') {
            if (after_dot || base == 0) return false;
            after_dot = true;
            continue;
        }
        const char value = uppercase(*cursor);
        if (!((value >= 'A' && value <= 'Z') || (value >= '0' && value <= '9') ||
              value == '_' || value == '-' || value == '$' || value == '~')) return false;
        if (!after_dot) {
            if (base >= 8) return false;
            output[base++] = static_cast<uint8_t>(value);
        } else {
            if (extension >= 3) return false;
            output[8 + extension++] = static_cast<uint8_t>(value);
        }
    }
    return base != 0;
}

bool locate_directory_slot(const Fat32Volume& volume, uint32_t directory_cluster,
                           const uint8_t name[11], DirectorySlot* slot) {
    if (slot == nullptr || volume.device.read_sector == nullptr ||
        volume.device.write_sector == nullptr || volume.sectors_per_cluster == 0 ||
        directory_cluster < 2 || directory_cluster >= volume.cluster_count + 2) return false;
    uint8_t sector[kSectorBytes];
    uint8_t fat[kSectorBytes];
    uint32_t cluster = directory_cluster;
    bool have_deleted_slot = false;
    DirectorySlot deleted_slot{};
    for (uint32_t chain = 0; chain < volume.cluster_count; ++chain) {
        for (uint32_t sector_index = 0; sector_index < volume.sectors_per_cluster; ++sector_index) {
            uint32_t lba;
            if (!cluster_lba(volume, cluster, sector_index, &lba) ||
                !volume.device.read_sector(volume.device.context, lba, sector)) return false;
            for (uint32_t offset = 0; offset < kSectorBytes; offset += 32) {
                const uint8_t* item = sector + offset;
                if (item[0] == 0) {
                    if (have_deleted_slot) *slot = deleted_slot;
                    else {
                        slot->lba = lba;
                        slot->offset = static_cast<uint16_t>(offset);
                        slot->old_first_cluster = 0;
                        slot->existed = false;
                    }
                    return true;
                }
                if (item[0] == 0xe5) {
                    if (!have_deleted_slot) {
                        deleted_slot.lba = lba;
                        deleted_slot.offset = static_cast<uint16_t>(offset);
                        deleted_slot.old_first_cluster = 0;
                        deleted_slot.existed = false;
                        have_deleted_slot = true;
                    }
                    continue;
                }
                if ((item[11] & 0x0f) == 0x0f || (item[11] & 0x08) != 0) continue;
                bool matches = true;
                for (uint32_t index = 0; index < 11; ++index) {
                    if (uppercase(static_cast<char>(item[index])) != static_cast<char>(name[index])) {
                        matches = false;
                        break;
                    }
                }
                if (matches) {
                    if ((item[11] & 0x10) != 0) return false;
                    slot->lba = lba;
                    slot->offset = static_cast<uint16_t>(offset);
                    slot->old_first_cluster =
                        (static_cast<uint32_t>(read_u16(item + 20)) << 16) | read_u16(item + 26);
                    slot->existed = true;
                    return true;
                }
            }
        }
        uint32_t following;
        if (!next_cluster(volume, cluster, fat, &following)) return false;
        if (following >= kFatEndOfChain) {
            if (have_deleted_slot) {
                *slot = deleted_slot;
                return true;
            }
            return false;
        }
        if (following < 2 || following == kFatBadCluster || following >= volume.cluster_count + 2)
            return false;
        cluster = following;
    }
    return false;
}

bool write_fat_entry(const Fat32Volume& volume, uint32_t cluster, uint32_t value) {
    if (volume.device.read_sector == nullptr || volume.device.write_sector == nullptr ||
        cluster >= volume.cluster_count + 2) return false;
    const uint32_t byte_offset = cluster * 4;
    const uint32_t fat_sector = byte_offset / kSectorBytes;
    if (fat_sector >= volume.sectors_per_fat) return false;
    uint8_t sector[kSectorBytes];
    for (uint32_t copy = 0; copy < volume.fat_count; ++copy) {
        const uint64_t lba = volume.volume_start_lba + volume.fat_start_lba +
            static_cast<uint64_t>(copy) * volume.sectors_per_fat + fat_sector;
        if (lba >= volume.device.sector_count ||
            !volume.device.read_sector(volume.device.context, lba, sector)) return false;
        const uint32_t offset = byte_offset % kSectorBytes;
        const uint32_t old = read_u32(sector + offset);
        const uint32_t updated = (old & 0xf0000000u) | (value & 0x0fffffffu);
        sector[offset] = static_cast<uint8_t>(updated);
        sector[offset + 1] = static_cast<uint8_t>(updated >> 8);
        sector[offset + 2] = static_cast<uint8_t>(updated >> 16);
        sector[offset + 3] = static_cast<uint8_t>(updated >> 24);
        if (!volume.device.write_sector(volume.device.context, lba, sector)) return false;
    }
    return true;
}

bool find_free_cluster(const Fat32Volume& volume, uint32_t* free_cluster) {
    if (free_cluster == nullptr || volume.device.read_sector == nullptr ||
        volume.sectors_per_fat == 0) return false;
    uint8_t sector[kSectorBytes];
    uint32_t cached_fat_sector = 0xffffffffu;
    for (uint32_t cluster = 2; cluster < volume.cluster_count + 2; ++cluster) {
        const uint32_t offset = cluster * 4;
        const uint32_t fat_sector = offset / kSectorBytes;
        if (fat_sector >= volume.sectors_per_fat) return false;
        if (fat_sector != cached_fat_sector) {
            const uint64_t lba = volume.volume_start_lba + volume.fat_start_lba + fat_sector;
            if (lba >= volume.device.sector_count ||
                !volume.device.read_sector(volume.device.context, lba, sector)) return false;
            cached_fat_sector = fat_sector;
        }
        if ((read_u32(sector + (offset % kSectorBytes)) & 0x0fffffffu) == 0) {
            *free_cluster = cluster;
            return true;
        }
    }
    return false;
}

bool write_cluster_data(const Fat32Volume& volume, uint32_t cluster,
                        const uint8_t* data, uint32_t size) {
    if (volume.device.write_sector == nullptr || volume.device.sector_size != kSectorBytes ||
        volume.sectors_per_cluster == 0 ||
        size > static_cast<uint64_t>(volume.sectors_per_cluster) * kSectorBytes) return false;
    uint8_t sector[kSectorBytes];
    uint32_t consumed = 0;
    for (uint32_t index = 0; index < volume.sectors_per_cluster; ++index) {
        for (uint32_t byte = 0; byte < kSectorBytes; ++byte) {
            sector[byte] = consumed < size ? data[consumed++] : 0;
        }
        uint32_t lba;
        if (!cluster_lba(volume, cluster, index, &lba) ||
            !volume.device.write_sector(volume.device.context, lba, sector)) return false;
    }
    return true;
}

void release_cluster_chain(const Fat32Volume& volume, uint32_t cluster) {
    uint8_t fat[kSectorBytes];
    for (uint32_t count = 0; count < volume.cluster_count; ++count) {
        if (cluster < 2 || cluster >= volume.cluster_count + 2) return;
        uint32_t following;
        if (!next_cluster(volume, cluster, fat, &following)) return;
        if (!write_fat_entry(volume, cluster, 0)) return;
        if (following >= kFatEndOfChain) return;
        if (following < 2 || following == kFatBadCluster || following >= volume.cluster_count + 2) return;
        cluster = following;
    }
}

} // namespace

bool fat32_mount(const BlockDevice& device, Fat32Volume* volume) {
    if (volume == nullptr || device.read_sector == nullptr || device.sector_size != kSectorBytes ||
        device.sector_count == 0) return false;

    uint8_t boot[kSectorBytes];
    uint64_t volume_start_lba;
    uint64_t available_sectors;
    if (!find_volume_start(device, boot, &volume_start_lba, &available_sectors) ||
        !device.read_sector(device.context, volume_start_lba, boot) || !looks_like_fat32(boot)) return false;

    const uint16_t bytes_per_sector = read_u16(boot + 11);
    const uint8_t sectors_per_cluster = boot[13];
    const uint16_t reserved_sectors = read_u16(boot + 14);
    const uint8_t fat_count = boot[16];
    const uint16_t root_entries = read_u16(boot + 17);
    const uint16_t total_sectors_16 = read_u16(boot + 19);
    const uint32_t total_sectors_32 = read_u32(boot + 32);
    const uint32_t sectors_per_fat = read_u32(boot + 36);
    const uint32_t root_cluster = read_u32(boot + 44) & 0x0fffffffu;
    const uint32_t total_sectors = total_sectors_16 != 0 ? total_sectors_16 : total_sectors_32;

    if (bytes_per_sector != kSectorBytes || sectors_per_cluster == 0 ||
        (sectors_per_cluster & (sectors_per_cluster - 1)) != 0 ||
        reserved_sectors == 0 || fat_count == 0 || sectors_per_fat == 0 ||
        root_entries != 0 || total_sectors == 0 || root_cluster < 2 ||
        total_sectors > available_sectors) return false;

    const uint64_t first_data = static_cast<uint64_t>(reserved_sectors) +
        static_cast<uint64_t>(fat_count) * sectors_per_fat;
    if (first_data >= total_sectors || first_data > 0xffffffffu) return false;
    const uint32_t clusters = static_cast<uint32_t>((total_sectors - first_data) / sectors_per_cluster);
    // FAT32 is identified by its data-cluster count, not the optional type label.
    if (clusters < 65525 || clusters > 0x0ffffff5u || root_cluster >= clusters + 2) return false;

    volume->device = device;
    volume->volume_start_lba = volume_start_lba;
    volume->volume_sector_count = total_sectors;
    volume->fat_start_lba = reserved_sectors;
    volume->data_start_lba = static_cast<uint32_t>(first_data);
    volume->sectors_per_fat = sectors_per_fat;
    volume->root_cluster = root_cluster;
    volume->cluster_count = clusters;
    volume->sectors_per_cluster = sectors_per_cluster;
    volume->fat_count = fat_count;
    return true;
}

bool fat32_list_root(const Fat32Volume& volume, Fat32DirectoryCallback callback, void* context) {
    return fat32_list_directory(volume, volume.root_cluster, callback, context);
}

bool fat32_list_directory(const Fat32Volume& volume, uint32_t directory_cluster,
                          Fat32DirectoryCallback callback, void* context) {
    if (callback == nullptr || volume.device.read_sector == nullptr || volume.sectors_per_cluster == 0 ||
        directory_cluster < 2 || directory_cluster >= volume.cluster_count + 2) return false;
    uint8_t data[kSectorBytes];
    uint8_t fat[kSectorBytes];
    uint32_t cluster = directory_cluster;

    for (uint32_t chain = 0; chain < volume.cluster_count; ++chain) {
        for (uint32_t sector_index = 0; sector_index < volume.sectors_per_cluster; ++sector_index) {
            uint32_t lba;
            if (!cluster_lba(volume, cluster, sector_index, &lba) ||
                !volume.device.read_sector(volume.device.context, lba, data)) return false;
            for (uint32_t offset = 0; offset < kSectorBytes; offset += 32) {
                const uint8_t* item = data + offset;
                if (item[0] == 0) return true;
                if (item[0] == 0xe5 || item[11] == 0x0f || (item[11] & 0x08) != 0) continue;

                Fat32DirectoryEntry entry{};
                decode_short_name(item, entry.name);
                entry.is_directory = (item[11] & 0x10) != 0;
                entry.first_cluster = (static_cast<uint32_t>(read_u16(item + 20)) << 16) |
                                      read_u16(item + 26);
                entry.size = read_u32(item + 28);
                if (!callback(context, entry)) return false;
            }
        }

        uint32_t following;
        if (!next_cluster(volume, cluster, fat, &following)) return false;
        if (following >= kFatEndOfChain) return true;
        if (following == 0 || following == 1 || following == kFatBadCluster ||
            following >= volume.cluster_count + 2) return false;
        cluster = following;
    }
    return false; // A chain longer than the volume's cluster count is corrupt.
}

namespace {
bool short_name_equal(const char* left, const char* right) {
    while (*left != '\0' && *right != '\0') {
        if (uppercase(*left) != uppercase(*right)) return false;
        ++left;
        ++right;
    }
    return *left == '\0' && *right == '\0';
}

struct FileLookup {
    const char* name;
    Fat32DirectoryEntry* entry;
    bool found;
};

bool find_file_callback(void* context, const Fat32DirectoryEntry& entry) {
    auto& lookup = *static_cast<FileLookup*>(context);
    if (!entry.is_directory && short_name_equal(entry.name, lookup.name)) {
        *lookup.entry = entry;
        lookup.found = true;
    }
    return true;
}
}

bool fat32_find_file(const Fat32Volume& volume, uint32_t directory_cluster,
                     const char* short_name, Fat32DirectoryEntry* entry) {
    if (short_name == nullptr || entry == nullptr) return false;
    FileLookup lookup{short_name, entry, false};
    if (!fat32_list_directory(volume, directory_cluster, find_file_callback, &lookup)) return false;
    return lookup.found;
}

bool fat32_read_file(const Fat32Volume& volume, const Fat32DirectoryEntry& entry,
                     uint8_t* output, uint32_t output_capacity, uint32_t* bytes_read) {
    if (bytes_read == nullptr || (entry.size != 0 && output == nullptr) || entry.is_directory ||
        entry.size > output_capacity || volume.device.read_sector == nullptr ||
        volume.device.sector_size != kSectorBytes || volume.sectors_per_cluster == 0 ||
        volume.cluster_count == 0) return false;
    *bytes_read = 0;
    if (entry.size == 0) {
        return entry.first_cluster == 0 ||
               (entry.first_cluster >= 2 && entry.first_cluster < volume.cluster_count + 2);
    }
    if (entry.first_cluster < 2 || entry.first_cluster >= volume.cluster_count + 2) return false;

    uint8_t sector[kSectorBytes];
    uint8_t fat_sector[kSectorBytes];
    uint32_t cluster = entry.first_cluster;
    uint32_t remaining = entry.size;
    for (uint32_t chain_index = 0; remaining != 0 && chain_index < volume.cluster_count; ++chain_index) {
        for (uint32_t sector_index = 0;
             sector_index < volume.sectors_per_cluster && remaining != 0; ++sector_index) {
            uint32_t lba;
            if (!cluster_lba(volume, cluster, sector_index, &lba) ||
                !volume.device.read_sector(volume.device.context, lba, sector)) return false;
            const uint32_t amount = remaining < kSectorBytes ? remaining : kSectorBytes;
            for (uint32_t byte = 0; byte < amount; ++byte) output[*bytes_read + byte] = sector[byte];
            *bytes_read += amount;
            remaining -= amount;
        }
        if (remaining == 0) return true;

        uint32_t following;
        if (!next_cluster(volume, cluster, fat_sector, &following) || following < 2 ||
            following == kFatBadCluster || following >= volume.cluster_count + 2 ||
            following >= kFatEndOfChain) return false;
        cluster = following;
    }
    return false;
}

bool fat32_write_file(const Fat32Volume& volume, uint32_t directory_cluster,
                      const char* short_name, const uint8_t* data, uint32_t size) {
    if (data == nullptr || size == 0 || volume.device.write_sector == nullptr ||
        volume.device.read_sector == nullptr || volume.device.sector_size != kSectorBytes ||
        volume.sectors_per_cluster == 0 ||
        size > static_cast<uint64_t>(volume.sectors_per_cluster) * kSectorBytes) return false;
    uint8_t name[11];
    if (!encode_short_name(short_name, name)) return false;

    DirectorySlot slot{};
    if (!locate_directory_slot(volume, directory_cluster, name, &slot)) return false;
    uint32_t new_cluster;
    if (!find_free_cluster(volume, &new_cluster) ||
        !write_fat_entry(volume, new_cluster, kFatEndOfChain)) return false;
    if (!write_cluster_data(volume, new_cluster, data, size)) {
        write_fat_entry(volume, new_cluster, 0);
        return false;
    }

    uint8_t sector[kSectorBytes];
    if (!volume.device.read_sector(volume.device.context, slot.lba, sector)) {
        write_fat_entry(volume, new_cluster, 0);
        return false;
    }
    uint8_t* entry = sector + slot.offset;
    for (uint32_t i = 0; i < 11; ++i) entry[i] = name[i];
    entry[11] = 0x20; // Archive attribute.
    for (uint32_t i = 12; i < 20; ++i) entry[i] = 0;
    entry[20] = static_cast<uint8_t>(new_cluster >> 16);
    entry[21] = static_cast<uint8_t>(new_cluster >> 24);
    entry[22] = entry[23] = entry[24] = entry[25] = 0;
    entry[26] = static_cast<uint8_t>(new_cluster);
    entry[27] = static_cast<uint8_t>(new_cluster >> 8);
    entry[28] = static_cast<uint8_t>(size);
    entry[29] = static_cast<uint8_t>(size >> 8);
    entry[30] = static_cast<uint8_t>(size >> 16);
    entry[31] = static_cast<uint8_t>(size >> 24);
    if (!volume.device.write_sector(volume.device.context, slot.lba, sector)) {
        write_fat_entry(volume, new_cluster, 0);
        return false;
    }
    if (slot.existed) release_cluster_chain(volume, slot.old_first_cluster);
    return true;
}

} // namespace annyos::storage
