#include "annyos/e1000.hpp"

namespace annyos::network {
namespace {

constexpr uint16_t kPciAddressPort = 0x0cf8;
constexpr uint16_t kPciDataPort = 0x0cfc;
constexpr uint32_t kPciEnable = 0x80000000u;
constexpr uint16_t kIntelVendor = 0x8086;
constexpr uint16_t kE1000Device = 0x100e; // 82540EM, including QEMU's e1000.
constexpr uint16_t kPciCommandMemory = 1u << 1;
constexpr uint16_t kPciCommandBusMaster = 1u << 2;

constexpr uint32_t kRegCtrl = 0x0000;
constexpr uint32_t kRegStatus = 0x0008;
constexpr uint32_t kRegInterruptMaskClear = 0x00d8;
constexpr uint32_t kRegReceiveControl = 0x0100;
constexpr uint32_t kRegTransmitControl = 0x0400;
constexpr uint32_t kRegTransmitIp = 0x0410;
constexpr uint32_t kRegRxBaseLow = 0x2800;
constexpr uint32_t kRegRxBaseHigh = 0x2804;
constexpr uint32_t kRegRxLength = 0x2808;
constexpr uint32_t kRegRxHead = 0x2810;
constexpr uint32_t kRegRxTail = 0x2818;
constexpr uint32_t kRegTxBaseLow = 0x3800;
constexpr uint32_t kRegTxBaseHigh = 0x3804;
constexpr uint32_t kRegTxLength = 0x3808;
constexpr uint32_t kRegTxHead = 0x3810;
constexpr uint32_t kRegTxTail = 0x3818;
constexpr uint32_t kRegReceiveAddressLow = 0x5400;
constexpr uint32_t kRegReceiveAddressHigh = 0x5404;

constexpr uint32_t kControlReset = 1u << 26;
constexpr uint32_t kReceiveEnable = 1u << 1;
constexpr uint32_t kReceiveBroadcast = 1u << 15;
constexpr uint32_t kReceiveStripCrc = 1u << 26;
constexpr uint32_t kTransmitEnable = 1u << 1;
constexpr uint32_t kTransmitPadShort = 1u << 3;
constexpr uint32_t kTxDescriptorEnd = 1u << 0;
constexpr uint32_t kTxDescriptorInsertCrc = 1u << 1;
constexpr uint32_t kTxDescriptorReportStatus = 1u << 3;
constexpr uint8_t kRxDescriptorDone = 1u << 0;
constexpr uint8_t kRxDescriptorEndOfPacket = 1u << 1;
constexpr uint8_t kTxDescriptorDone = 1u << 0;

constexpr uint32_t kRxDescriptorCount = 128;
constexpr uint32_t kTxDescriptorCount = 64;
constexpr uint32_t kDescriptorBytes = 16;
constexpr uint32_t kDmaRxDescriptorsOffset = 0;
constexpr uint32_t kDmaTxDescriptorsOffset = 4096;
constexpr uint32_t kDmaRxBuffersOffset = 8192;
constexpr uint32_t kDmaBufferBytes = 2048;
constexpr uint32_t kDmaRxBuffersBytes = kRxDescriptorCount * kDmaBufferBytes;
constexpr uint32_t kDmaTxBuffersOffset = kDmaRxBuffersOffset + kDmaRxBuffersBytes;
constexpr uint32_t kDmaTxBuffersBytes = kTxDescriptorCount * kDmaBufferBytes;
constexpr uint32_t kRequiredDmaBytes = kDmaTxBuffersOffset + kDmaTxBuffersBytes;
constexpr uint32_t kMaximumEthernetFrameBytes = 1518;
constexpr uint32_t kPollLimit = 1000000;

struct __attribute__((packed)) RxDescriptor {
    uint64_t address;
    uint16_t length;
    uint16_t checksum;
    uint8_t status;
    uint8_t errors;
    uint16_t special;
};

struct __attribute__((packed)) TxDescriptor {
    uint64_t address;
    uint16_t length;
    uint8_t checksum_offset;
    uint8_t command;
    uint8_t status;
    uint8_t checksum_start;
    uint16_t special;
};

static_assert(sizeof(RxDescriptor) == kDescriptorBytes, "e1000 RX descriptor layout changed");
static_assert(sizeof(TxDescriptor) == kDescriptorBytes, "e1000 TX descriptor layout changed");

volatile uint32_t* g_registers;
uint64_t g_dma_physical;
uint8_t* g_dma_virtual;
RxDescriptor* g_rx_descriptors;
TxDescriptor* g_tx_descriptors;
MacAddress g_mac{};
uint32_t g_rx_index;
uint32_t g_tx_index;
bool g_ready;

void out32(uint16_t port, uint32_t value) {
    __asm__ volatile("outl %0, %1" : : "a"(value), "Nd"(port) : "memory");
}

uint32_t in32(uint16_t port) {
    uint32_t value;
    __asm__ volatile("inl %1, %0" : "=a"(value) : "Nd"(port) : "memory");
    return value;
}

void pci_select(uint8_t bus, uint8_t device, uint8_t function, uint8_t offset) {
    out32(kPciAddressPort, kPciEnable | (static_cast<uint32_t>(bus) << 16) |
        (static_cast<uint32_t>(device) << 11) | (static_cast<uint32_t>(function) << 8) |
        (offset & 0xfcu));
}

uint32_t pci_read32(uint8_t bus, uint8_t device, uint8_t function, uint8_t offset) {
    pci_select(bus, device, function, offset);
    return in32(kPciDataPort);
}

void pci_write16(uint8_t bus, uint8_t device, uint8_t function,
                 uint8_t offset, uint16_t value) {
    pci_select(bus, device, function, offset);
    __asm__ volatile("outw %0, %1" : : "a"(value), "Nd"(
        static_cast<uint16_t>(kPciDataPort + (offset & 2u))) : "memory");
}

struct PciLocation {
    uint8_t bus;
    uint8_t device;
    uint8_t function;
    uint32_t mmio_base;
};

bool find_device(PciLocation* result) {
    if (result == nullptr) return false;
    for (uint32_t bus = 0; bus < 256; ++bus) {
        for (uint32_t device = 0; device < 32; ++device) {
            const uint32_t header = pci_read32(static_cast<uint8_t>(bus),
                static_cast<uint8_t>(device), 0, 0x0c);
            const uint8_t functions = ((header >> 23) & 1u) != 0 ? 8 : 1;
            for (uint32_t function = 0; function < functions; ++function) {
                const uint8_t b = static_cast<uint8_t>(bus);
                const uint8_t d = static_cast<uint8_t>(device);
                const uint8_t f = static_cast<uint8_t>(function);
                const uint32_t id = pci_read32(b, d, f, 0x00);
                if (static_cast<uint16_t>(id) != kIntelVendor ||
                    static_cast<uint16_t>(id >> 16) != kE1000Device) continue;

                const uint32_t bar0 = pci_read32(b, d, f, 0x10);
                // This implementation supports only the 32-bit memory BAR of
                // the 82540EM class; reject I/O and 64-bit BAR encodings.
                if ((bar0 & 1u) != 0 || (bar0 & 6u) != 0) continue;
                const uint32_t base = bar0 & ~0x0fu;
                if (base == 0 || base > 0xffff0000u) continue;
                result->bus = b;
                result->device = d;
                result->function = f;
                result->mmio_base = base;
                return true;
            }
        }
    }
    return false;
}

uint32_t read_register(uint32_t offset) {
    __asm__ volatile("" : : : "memory");
    const uint32_t value = g_registers[offset / sizeof(uint32_t)];
    __asm__ volatile("" : : : "memory");
    return value;
}

void write_register(uint32_t offset, uint32_t value) {
    __asm__ volatile("" : : : "memory");
    g_registers[offset / sizeof(uint32_t)] = value;
    (void)g_registers[kRegStatus / sizeof(uint32_t)]; // flush posted MMIO writes
    __asm__ volatile("" : : : "memory");
}

bool valid_mac(const MacAddress& mac) {
    bool any_nonzero = false;
    bool all_ones = true;
    for (uint32_t index = 0; index < 6; ++index) {
        any_nonzero |= mac.bytes[index] != 0;
        all_ones &= mac.bytes[index] == 0xff;
    }
    return any_nonzero && !all_ones && (mac.bytes[0] & 1u) == 0;
}

void memory_barrier() { __asm__ volatile("" : : : "memory"); }

} // namespace

bool e1000_initialize(const BootInfo& boot_info) {
    g_ready = false;
    g_registers = nullptr;
    g_dma_virtual = nullptr;
    g_dma_physical = 0;
    g_rx_descriptors = nullptr;
    g_tx_descriptors = nullptr;
    g_mac = {};
    g_rx_index = 0;
    g_tx_index = 0;

    if (boot_info.dma_region == nullptr || boot_info.dma_region_size < kRequiredDmaBytes ||
        boot_info.dma_physical_address == 0 ||
        boot_info.dma_physical_address > 0xffffffffull - kRequiredDmaBytes ||
        (boot_info.dma_physical_address & 0xfffu) != 0 ||
        reinterpret_cast<uint64_t>(boot_info.dma_region) != boot_info.dma_physical_address) {
        return false;
    }

    PciLocation location{};
    if (!find_device(&location)) return false;
    uint16_t command = static_cast<uint16_t>(pci_read32(
        location.bus, location.device, location.function, 0x04));
    command |= kPciCommandMemory | kPciCommandBusMaster;
    pci_write16(location.bus, location.device, location.function, 0x04, command);
    const uint16_t checked = static_cast<uint16_t>(pci_read32(
        location.bus, location.device, location.function, 0x04));
    if ((checked & (kPciCommandMemory | kPciCommandBusMaster)) !=
        (kPciCommandMemory | kPciCommandBusMaster)) return false;

    g_registers = reinterpret_cast<volatile uint32_t*>(
        static_cast<uintptr_t>(location.mmio_base));
    g_dma_physical = boot_info.dma_physical_address;
    g_dma_virtual = static_cast<uint8_t*>(boot_info.dma_region);

    write_register(kRegInterruptMaskClear, 0xffffffffu); // polling only
    write_register(kRegReceiveControl, 0);
    write_register(kRegTransmitControl, 0);
    write_register(kRegCtrl, read_register(kRegCtrl) | kControlReset);
    uint32_t retries = kPollLimit;
    while (retries-- != 0 && (read_register(kRegCtrl) & kControlReset) != 0) {
        __asm__ volatile("pause");
    }
    if ((read_register(kRegCtrl) & kControlReset) != 0) {
        g_registers = nullptr;
        return false;
    }

    // Reset clears interrupt masking and the receive/transmit state.
    write_register(kRegInterruptMaskClear, 0xffffffffu);
    for (uint32_t index = 0; index < 6; ++index) {
        const uint32_t register_value = read_register(
            (index < 4 ? kRegReceiveAddressLow : kRegReceiveAddressHigh));
        const uint32_t byte_shift = (index & 3u) * 8u;
        g_mac.bytes[index] = static_cast<uint8_t>(register_value >> byte_shift);
    }
    if (!valid_mac(g_mac)) {
        g_registers = nullptr;
        return false;
    }

    g_rx_descriptors = reinterpret_cast<RxDescriptor*>(g_dma_virtual + kDmaRxDescriptorsOffset);
    g_tx_descriptors = reinterpret_cast<TxDescriptor*>(g_dma_virtual + kDmaTxDescriptorsOffset);
    for (uint32_t index = 0; index < kRxDescriptorCount; ++index) {
        const uint64_t buffer = g_dma_physical + kDmaRxBuffersOffset + index * kDmaBufferBytes;
        g_rx_descriptors[index] = {buffer, 0, 0, 0, 0, 0};
    }
    for (uint32_t index = 0; index < kTxDescriptorCount; ++index) {
        const uint64_t buffer = g_dma_physical + kDmaTxBuffersOffset + index * kDmaBufferBytes;
        g_tx_descriptors[index] = {buffer, 0, 0, 0, kTxDescriptorDone, 0, 0};
    }

    const uint64_t rx_ring = g_dma_physical + kDmaRxDescriptorsOffset;
    const uint64_t tx_ring = g_dma_physical + kDmaTxDescriptorsOffset;
    write_register(kRegRxBaseLow, static_cast<uint32_t>(rx_ring));
    write_register(kRegRxBaseHigh, static_cast<uint32_t>(rx_ring >> 32));
    write_register(kRegRxLength, kRxDescriptorCount * kDescriptorBytes);
    write_register(kRegRxHead, 0);
    write_register(kRegRxTail, kRxDescriptorCount - 1);
    write_register(kRegTxBaseLow, static_cast<uint32_t>(tx_ring));
    write_register(kRegTxBaseHigh, static_cast<uint32_t>(tx_ring >> 32));
    write_register(kRegTxLength, kTxDescriptorCount * kDescriptorBytes);
    write_register(kRegTxHead, 0);
    write_register(kRegTxTail, 0);
    write_register(kRegTransmitIp, 0x0060200au);
    write_register(kRegTransmitControl, kTransmitEnable | kTransmitPadShort |
                   (0x10u << 4) | (0x40u << 12));
    memory_barrier();
    write_register(kRegReceiveControl,
                   kReceiveEnable | kReceiveBroadcast | kReceiveStripCrc);

    g_ready = true;
    return true;
}

bool e1000_ready() { return g_ready; }

MacAddress e1000_mac_address() { return g_mac; }

bool e1000_send(const uint8_t* frame, uint16_t length) {
    if (!g_ready || frame == nullptr || length < 14 || length > kMaximumEthernetFrameBytes) {
        return false;
    }
    TxDescriptor& descriptor = g_tx_descriptors[g_tx_index];
    uint32_t retries = kPollLimit;
    while (retries-- != 0 && (descriptor.status & kTxDescriptorDone) == 0) {
        __asm__ volatile("pause");
    }
    if ((descriptor.status & kTxDescriptorDone) == 0) return false;

    uint8_t* buffer = g_dma_virtual + kDmaTxBuffersOffset + g_tx_index * kDmaBufferBytes;
    for (uint32_t index = 0; index < length; ++index) buffer[index] = frame[index];
    descriptor.length = length;
    descriptor.command = kTxDescriptorEnd | kTxDescriptorInsertCrc | kTxDescriptorReportStatus;
    descriptor.status = 0;
    memory_barrier();
    g_tx_index = (g_tx_index + 1) % kTxDescriptorCount;
    write_register(kRegTxTail, g_tx_index);
    return true;
}

uint32_t e1000_poll(ReceiveFrame receive, void* context, uint32_t frame_budget) {
    if (!g_ready || receive == nullptr) return 0;
    uint32_t frames = 0;
    while (frames < frame_budget) {
        RxDescriptor& descriptor = g_rx_descriptors[g_rx_index];
        memory_barrier();
        if ((descriptor.status & kRxDescriptorDone) == 0) break;
        const uint16_t length = descriptor.length;
        const bool complete = (descriptor.status & kRxDescriptorEndOfPacket) != 0;
        const bool valid = complete && descriptor.errors == 0 && length >= 14 &&
                           length <= kMaximumEthernetFrameBytes;
        if (valid) {
            const uint8_t* buffer = g_dma_virtual + kDmaRxBuffersOffset +
                                    g_rx_index * kDmaBufferBytes;
            receive(context, buffer, length);
            ++frames;
        }
        descriptor.length = 0;
        descriptor.checksum = 0;
        descriptor.status = 0;
        descriptor.errors = 0;
        memory_barrier();
        write_register(kRegRxTail, g_rx_index);
        g_rx_index = (g_rx_index + 1) % kRxDescriptorCount;
    }
    return frames;
}

} // namespace annyos::network
