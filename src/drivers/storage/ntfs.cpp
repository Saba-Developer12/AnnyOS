#include "annyos/ntfs.hpp"

namespace annyos::storage {
namespace {

constexpr uint32_t kMinimumSectorBytes = 512;
// Current AnnyOS ATA PIO exposes 512-byte logical sectors.
constexpr uint32_t kMaximumSectorBytes = 512;

uint16_t read_u16(const uint8_t* bytes) {
    return static_cast<uint16_t>(bytes[0]) |
           (static_cast<uint16_t>(bytes[1]) << 8);
}

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

bool is_power_of_two(uint32_t value) {
    return value != 0 && (value & (value - 1)) == 0;
}

bool probe_boot_sector(const BlockDevice& device, uint64_t start_lba,
                       NtfsVolumeInfo* volume) {
    if (device.read_sector == nullptr || volume == nullptr ||
        device.sector_size < kMinimumSectorBytes ||
        device.sector_size > kMaximumSectorBytes ||
        start_lba >= device.sector_count) return false;

    uint8_t sector[kMaximumSectorBytes];
    if (!device.read_sector(device.context, start_lba, sector) ||
        sector[510] != 0x55 || sector[511] != 0xaa) return false;
    static constexpr uint8_t kNtfsOemId[8] = {'N', 'T', 'F', 'S', ' ', ' ', ' ', ' '};
    for (uint32_t index = 0; index < sizeof(kNtfsOemId); ++index) {
        if (sector[3 + index] != kNtfsOemId[index]) return false;
    }

    const uint32_t bytes_per_sector = read_u16(sector + 11);
    const uint32_t sectors_per_cluster = sector[13];
    const uint64_t sector_count = read_u64(sector + 40);
    const uint64_t mft_cluster = read_u64(sector + 48);
    const uint64_t mirror_cluster = read_u64(sector + 56);
    const int8_t encoded_record_size = static_cast<int8_t>(sector[64]);
    if (!is_power_of_two(bytes_per_sector) || bytes_per_sector != device.sector_size ||
        !is_power_of_two(sectors_per_cluster) || sectors_per_cluster > 128 ||
        sector_count == 0 || sector_count > device.sector_count - start_lba) return false;

    const uint64_t cluster_count = sector_count / sectors_per_cluster;
    if (mft_cluster >= cluster_count || mirror_cluster >= cluster_count) return false;

    const uint64_t cluster_size = static_cast<uint64_t>(bytes_per_sector) * sectors_per_cluster;
    uint64_t record_size = 0;
    if (encoded_record_size > 0) {
        record_size = static_cast<uint64_t>(encoded_record_size) * cluster_size;
    } else if (encoded_record_size < 0) {
        const uint32_t shift = static_cast<uint32_t>(-static_cast<int32_t>(encoded_record_size));
        if (shift >= 32) return false;
        record_size = static_cast<uint64_t>(1u) << shift;
    }
    if (cluster_size > 0xffffffffu || record_size < 512 || record_size > 65536) return false;

    volume->partition_start_lba = start_lba;
    volume->sector_count = sector_count;
    volume->mft_cluster = mft_cluster;
    volume->mft_mirror_cluster = mirror_cluster;
    volume->serial_number = read_u64(sector + 72);
    volume->bytes_per_sector = bytes_per_sector;
    volume->cluster_size_bytes = static_cast<uint32_t>(cluster_size);
    volume->file_record_size_bytes = static_cast<uint32_t>(record_size);
    volume->sectors_per_cluster = static_cast<uint8_t>(sectors_per_cluster);
    return true;
}

} // namespace

bool ntfs_probe(const BlockDevice& device, NtfsVolumeInfo* volume) {
    if (volume == nullptr || device.read_sector == nullptr || device.sector_count == 0 ||
        device.sector_size < kMinimumSectorBytes || device.sector_size > kMaximumSectorBytes) {
        return false;
    }

    // NTFS can occupy a whole device or an MBR partition. Require a valid NTFS
    // OEM identifier at the candidate boot sector so type 0x07 alone is not
    // mistaken for NTFS (it is also used by other formats).
    if (probe_boot_sector(device, 0, volume)) return true;

    uint8_t mbr[kMaximumSectorBytes];
    if (!device.read_sector(device.context, 0, mbr) ||
        mbr[510] != 0x55 || mbr[511] != 0xaa) return false;
    for (uint32_t index = 0; index < 4; ++index) {
        const uint8_t* partition = mbr + 446 + index * 16;
        if (partition[4] != 0x07) continue;
        const uint64_t start_lba = read_u32(partition + 8);
        const uint64_t sectors = read_u32(partition + 12);
        if (start_lba == 0 || sectors == 0 || start_lba >= device.sector_count ||
            sectors > device.sector_count - start_lba) continue;
        if (probe_boot_sector(device, start_lba, volume)) return true;
    }
    return false;
}

} // namespace annyos::storage
