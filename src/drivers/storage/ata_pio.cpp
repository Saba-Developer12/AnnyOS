#include "annyos/ata_pio.hpp"

namespace annyos::storage {
namespace {
constexpr uint16_t kData = 0;
constexpr uint16_t kSectorCount = 2;
constexpr uint16_t kLbaLow = 3;
constexpr uint16_t kLbaMid = 4;
constexpr uint16_t kLbaHigh = 5;
constexpr uint16_t kDrive = 6;
constexpr uint16_t kStatus = 7;
constexpr uint16_t kCommand = 7;
constexpr uint8_t kBusy = 0x80;
constexpr uint8_t kDeviceFault = 0x20;
constexpr uint8_t kErrorFlag = 0x01;
constexpr uint8_t kDataRequest = 0x08;
constexpr uint32_t kPollLimit = 1000000;

uint8_t in8(uint16_t port) {
    uint8_t value;
    __asm__ volatile("inb %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

void out8(uint16_t port, uint8_t value) {
    __asm__ volatile("outb %0, %1" : : "a"(value), "Nd"(port));
}

uint16_t in16(uint16_t port) {
    uint16_t value;
    __asm__ volatile("inw %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

void io_delay(AtaPioDevice& device) {
    (void)in8(device.control_port);
    (void)in8(device.control_port);
    (void)in8(device.control_port);
    (void)in8(device.control_port);
}

bool wait_status(AtaPioDevice& device, bool require_data) {
    for (uint32_t attempt = 0; attempt < kPollLimit; ++attempt) {
        const uint8_t status = in8(device.io_base + kStatus);
        if (status == 0 || status == 0xff) return false;
        if ((status & kBusy) != 0) continue;
        if ((status & (kErrorFlag | kDeviceFault)) != 0) return false;
        if (require_data && (status & kDataRequest) == 0) continue;
        return true;
    }
    return false;
}
}

bool ata_pio_initialize(uint8_t channel, uint8_t drive, AtaPioDevice* state, BlockDevice* device) {
    if (channel > 1 || drive > 1 || state == nullptr || device == nullptr) return false;
    state->io_base = channel == 0 ? 0x1f0 : 0x170;
    state->control_port = channel == 0 ? 0x3f6 : 0x376;
    state->drive = drive;
    state->sector_count = 0;

    out8(state->io_base + kDrive, static_cast<uint8_t>(0xa0 | (drive << 4)));
    io_delay(*state);
    out8(state->io_base + kSectorCount, 0);
    out8(state->io_base + kLbaLow, 0);
    out8(state->io_base + kLbaMid, 0);
    out8(state->io_base + kLbaHigh, 0);
    out8(state->io_base + kCommand, 0xec); // IDENTIFY DEVICE
    if (in8(state->io_base + kStatus) == 0 || !wait_status(*state, true)) return false;

    uint16_t identify[256];
    for (uint32_t word = 0; word < 256; ++word) identify[word] = in16(state->io_base + kData);
    const uint32_t sectors = static_cast<uint32_t>(identify[60]) |
                             (static_cast<uint32_t>(identify[61]) << 16);
    if (sectors == 0) return false;
    state->sector_count = sectors;
    device->context = state;
    device->sector_count = sectors;
    device->sector_size = 512;
    device->read_sector = ata_pio_read_sector;
    return true;
}

bool ata_pio_read_sector(void* context, uint64_t lba, uint8_t* destination) {
    auto* state = static_cast<AtaPioDevice*>(context);
    if (state == nullptr || destination == nullptr || lba >= state->sector_count ||
        lba > 0x0fffffffull) return false;
    if (!wait_status(*state, false)) return false;

    out8(state->io_base + kDrive, static_cast<uint8_t>(0xe0 | (state->drive << 4) | ((lba >> 24) & 0x0f)));
    io_delay(*state);
    out8(state->io_base + kSectorCount, 1);
    out8(state->io_base + kLbaLow, static_cast<uint8_t>(lba));
    out8(state->io_base + kLbaMid, static_cast<uint8_t>(lba >> 8));
    out8(state->io_base + kLbaHigh, static_cast<uint8_t>(lba >> 16));
    out8(state->io_base + kCommand, 0x20); // READ SECTORS (28-bit LBA)
    if (!wait_status(*state, true)) return false;

    for (uint32_t word = 0; word < 256; ++word) {
        const uint16_t value = in16(state->io_base + kData);
        destination[word * 2] = static_cast<uint8_t>(value);
        destination[word * 2 + 1] = static_cast<uint8_t>(value >> 8);
    }
    io_delay(*state);
    return true;
}

} // namespace annyos::storage

extern "C" bool annyos_ata_pio_initialize(uint8_t channel, uint8_t drive,
                                           annyos::storage::AtaPioDevice* state,
                                           annyos::storage::BlockDevice* device) {
    return annyos::storage::ata_pio_initialize(channel, drive, state, device);
}

extern "C" bool annyos_ata_pio_read_sector(void* context, uint64_t lba, uint8_t* destination) {
    return annyos::storage::ata_pio_read_sector(context, lba, destination);
}
