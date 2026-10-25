#include "annyos/apfs.hpp"

namespace annyos::storage {
namespace {

constexpr uint32_t kMinimumSectorBytes = 512;
// Current AnnyOS ATA PIO exposes 512-byte logical sectors.
constexpr uint32_t kMaximumSectorBytes = 512;
constexpr uint32_t kMaximumApfsBlockBytes = 1024 * 1024;
constexpr uint64_t kFletcherModulus = 0xffffffffull;

uint32_t read_u32(const uint8_t* bytes) {
    return static_cast<uint32_t>(bytes[0]) |
           (static_cast<uint32_t>(bytes[1]) << 8) |
           (static_cast<uint32_t>(bytes[2]) << 16) |
           (static_cast<uint32_t>(bytes[3]) << 24);
}

uint64_t read_u64(const uint8_t* bytes) {
    return static_cast<uint64_t>(read_u32(bytes)) |
           (static_cast<uint64_t>(read_u32(bytes + 4)) << 32);
}

bool apfs_guid(const uint8_t* guid) {
    static constexpr uint8_t kApfsPartitionGuid[16] = {
        0xef, 0x57, 0x34, 0x7c, 0x00, 0x00, 0xaa, 0x11,
        0xaa, 0x11, 0x00, 0x30, 0x65, 0x43, 0xec, 0xac,
    };
    for (uint32_t index = 0; index < sizeof(kApfsPartitionGuid); ++index) {
        if (guid[index] != kApfsPartitionGuid[index]) return false;
    }
    return true;
}

bool has_nxsb(const uint8_t* sector) {
    return sector[32] == 'N' && sector[33] == 'X' &&
           sector[34] == 'S' && sector[35] == 'B';
}

bool checksum_valid(const BlockDevice& device, uint64_t start_lba,
                    uint32_t block_size, uint64_t stored_checksum) {
    if (device.read_sector == nullptr || device.sector_size < kMinimumSectorBytes ||
        device.sector_size > kMaximumSectorBytes || block_size < device.sector_size ||
        block_size > kMaximumApfsBlockBytes || block_size % device.sector_size != 0 ||
        block_size % 4 != 0) return false;

    uint8_t sector[kMaximumSectorBytes];
    uint64_t sum1 = 0;
    uint64_t sum2 = 0;
    const uint32_t sectors = block_size / device.sector_size;
    for (uint32_t sector_index = 0; sector_index < sectors; ++sector_index) {
        if (start_lba > UINT64_MAX - sector_index ||
            !device.read_sector(device.context, start_lba + sector_index, sector)) return false;
        const uint32_t block_offset = sector_index * device.sector_size;
        for (uint32_t offset = 0; offset + 4 <= device.sector_size; offset += 4) {
            if (block_offset + offset < 8) continue; // Exclude the checksum word.
            const uint32_t word = read_u32(sector + offset);
            sum1 = (sum1 + word) % kFletcherModulus;
            sum2 = (sum2 + sum1) % kFletcherModulus;
        }
    }
    const uint64_t low = kFletcherModulus - ((sum1 + sum2) % kFletcherModulus);
    const uint64_t high = kFletcherModulus - ((sum1 + low) % kFletcherModulus);
    return stored_checksum == ((high << 32) | low);
}

bool probe_container(const BlockDevice& device, uint64_t start_lba,
                    uint64_t partition_sectors, ApfsContainerInfo* container) {
    if (container == nullptr || device.read_sector == nullptr ||
        device.sector_size < kMinimumSectorBytes || device.sector_size > kMaximumSectorBytes ||
        start_lba >= device.sector_count || partition_sectors == 0 ||
        partition_sectors > device.sector_count - start_lba) return false;

    uint8_t sector[kMaximumSectorBytes];
    if (!device.read_sector(device.context, start_lba, sector) || !has_nxsb(sector)) return false;
    const uint32_t block_size = read_u32(sector + 36);
    const uint64_t block_count = read_u64(sector + 40);
    const uint64_t stored_checksum = read_u64(sector);
    if (block_size < device.sector_size || block_size > kMaximumApfsBlockBytes ||
        block_size % device.sector_size != 0 || block_size % 4 != 0 || block_count == 0 ||
        block_count > partition_sectors / (block_size / device.sector_size)) return false;
    if (!checksum_valid(device, start_lba, block_size, stored_checksum)) return false;

    container->partition_start_lba = start_lba;
    container->partition_sector_count = partition_sectors;
    container->block_count = block_count;
    container->next_transaction_id = read_u64(sector + 96);
    container->block_size = block_size;
    container->max_file_systems = read_u32(sector + 152);
    return true;
}

} // namespace

bool apfs_probe(const BlockDevice& device, ApfsContainerInfo* container) {
    if (container == nullptr || device.read_sector == nullptr || device.sector_count == 0 ||
        device.sector_size < kMinimumSectorBytes || device.sector_size > kMaximumSectorBytes) {
        return false;
    }

    uint8_t sector[kMaximumSectorBytes];
    if (!device.read_sector(device.context, 0, sector)) return false;
    if (has_nxsb(sector)) return probe_container(device, 0, device.sector_count, container);

    // APFS is normally carried in a GPT partition. Validate the basic GPT
    // bounds and only inspect entries whose type GUID is the APFS GUID.
    if (device.sector_count < 2 ||
        !device.read_sector(device.context, 1, sector) ||
        sector[0] != 'E' || sector[1] != 'F' || sector[2] != 'I' || sector[3] != ' ' ||
        sector[4] != 'P' || sector[5] != 'A' || sector[6] != 'R' || sector[7] != 'T') return false;
    const uint32_t header_size = read_u32(sector + 12);
    const uint64_t entries_lba = read_u64(sector + 72);
    const uint32_t entry_count = read_u32(sector + 80);
    const uint32_t entry_size = read_u32(sector + 84);
    if (header_size < 92 || header_size > device.sector_size || entry_count == 0 ||
        entry_count > 4096 || entry_size < 128 || entry_size > kMaximumApfsBlockBytes ||
        entry_size % 8 != 0 || entries_lba >= device.sector_count) return false;

    for (uint32_t index = 0; index < entry_count; ++index) {
        const uint64_t entry_byte_offset = static_cast<uint64_t>(index) * entry_size;
        const uint64_t entry_sector_delta = entry_byte_offset / device.sector_size;
        const uint32_t entry_offset = static_cast<uint32_t>(entry_byte_offset % device.sector_size);
        if (entry_offset + 48 > device.sector_size || entries_lba > UINT64_MAX - entry_sector_delta ||
            entries_lba + entry_sector_delta >= device.sector_count) return false;
        if (!device.read_sector(device.context, entries_lba + entry_sector_delta, sector)) return false;
        const uint8_t* entry = sector + entry_offset;
        if (!apfs_guid(entry)) continue;
        const uint64_t first_lba = read_u64(entry + 32);
        const uint64_t last_lba = read_u64(entry + 40);
        if (first_lba > last_lba || last_lba >= device.sector_count) continue;
        if (probe_container(device, first_lba, last_lba - first_lba + 1, container)) return true;
    }
    return false;
}

} // namespace annyos::storage
