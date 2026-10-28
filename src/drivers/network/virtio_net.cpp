#include "annyos/virtio_net.hpp"

namespace annyos::network {
namespace {

constexpr uint16_t kPciAddressPort = 0x0cf8;
constexpr uint16_t kPciDataPort = 0x0cfc;
constexpr uint32_t kPciEnable = 0x80000000u;
constexpr uint16_t kVirtioVendor = 0x1af4;
constexpr uint16_t kVirtioNetLegacyDevice = 0x1000;
constexpr uint16_t kPciCommandIo = 1u << 0;
constexpr uint16_t kPciCommandBusMaster = 1u << 2;

constexpr uint16_t kHostFeatures = 0x00;
constexpr uint16_t kGuestFeatures = 0x04;
constexpr uint16_t kQueuePfn = 0x08;
constexpr uint16_t kQueueSize = 0x0c;
constexpr uint16_t kQueueSelect = 0x0e;
constexpr uint16_t kQueueNotify = 0x10;
constexpr uint16_t kDeviceStatus = 0x12;
constexpr uint16_t kDeviceConfig = 0x14;
constexpr uint16_t kGuestPageSize = 0x28;
constexpr uint16_t kQueueAlign = 0x2c;

constexpr uint8_t kStatusAcknowledge = 1u << 0;
constexpr uint8_t kStatusDriver = 1u << 1;
constexpr uint8_t kStatusDriverOk = 1u << 2;
constexpr uint8_t kStatusFailed = 1u << 7;
constexpr uint32_t kFeatureMac = 1u << 5;

// Legacy virtio-pci devices expose a fixed queue size through QUEUE_SIZE.
// QEMU's legacy network device uses 256 entries; keep bounded storage for
// that maximum while using the actual per-queue size reported by hardware.
constexpr uint32_t kMaximumQueueLength = 256;
constexpr uint32_t kPageBytes = 4096;
constexpr uint32_t kQueueBytes = 16384;
constexpr uint32_t kRxQueueOffset = 0;
constexpr uint32_t kTxQueueOffset = kRxQueueOffset + kQueueBytes;
constexpr uint32_t kRxBuffersOffset = kTxQueueOffset + kQueueBytes;
constexpr uint32_t kFrameBufferBytes = 2048;
constexpr uint32_t kRxBuffersBytes = kMaximumQueueLength * kFrameBufferBytes;
constexpr uint32_t kTxBufferOffset = kRxBuffersOffset + kRxBuffersBytes;
constexpr uint32_t kRequiredDmaBytes = kTxBufferOffset + kFrameBufferBytes;
constexpr uint32_t kNetHeaderBytes = 10;
constexpr uint32_t kMaximumEthernetFrameBytes = 1518;

struct __attribute__((packed)) Descriptor {
    uint64_t address;
    uint32_t length;
    uint16_t flags;
    uint16_t next;
};
static_assert(sizeof(Descriptor) == 16, "virtqueue descriptor layout changed");

struct __attribute__((packed)) AvailableRing {
    uint16_t flags;
    volatile uint16_t index;
    uint16_t ring[kMaximumQueueLength];
};

struct __attribute__((packed)) UsedElement {
    uint32_t id;
    uint32_t length;
};

struct __attribute__((packed)) UsedRing {
    uint16_t flags;
    volatile uint16_t index;
    UsedElement ring[kMaximumQueueLength];
};

struct QueueView {
    Descriptor* descriptors;
    AvailableRing* available;
    UsedRing* used;
    uint16_t size;
    uint16_t last_used_index;
};

uint16_t g_io_base;
uint64_t g_dma_physical;
uint8_t* g_dma_virtual;
QueueView g_rx_queue{};
QueueView g_tx_queue{};
MacAddress g_mac{};
bool g_tx_pending;
bool g_ready;

void out8(uint16_t port, uint8_t value) {
    __asm__ volatile("outb %0, %1" : : "a"(value), "Nd"(port) : "memory");
}

uint8_t in8(uint16_t port) {
    uint8_t value;
    __asm__ volatile("inb %1, %0" : "=a"(value) : "Nd"(port) : "memory");
    return value;
}

void out16(uint16_t port, uint16_t value) {
    __asm__ volatile("outw %0, %1" : : "a"(value), "Nd"(port) : "memory");
}

uint16_t in16(uint16_t port) {
    uint16_t value;
    __asm__ volatile("inw %1, %0" : "=a"(value) : "Nd"(port) : "memory");
    return value;
}

void out32(uint16_t port, uint32_t value) {
    __asm__ volatile("outl %0, %1" : : "a"(value), "Nd"(port) : "memory");
}

uint32_t in32(uint16_t port) {
    uint32_t value;
    __asm__ volatile("inl %1, %0" : "=a"(value) : "Nd"(port) : "memory");
    return value;
}

void pci_write_address(uint8_t bus, uint8_t device, uint8_t function, uint8_t offset) {
    const uint32_t address = kPciEnable | (static_cast<uint32_t>(bus) << 16) |
        (static_cast<uint32_t>(device) << 11) | (static_cast<uint32_t>(function) << 8) |
        (offset & 0xfcu);
    out32(kPciAddressPort, address);
}

uint32_t pci_read32(uint8_t bus, uint8_t device, uint8_t function, uint8_t offset) {
    pci_write_address(bus, device, function, offset);
    return in32(kPciDataPort);
}

void pci_write16(uint8_t bus, uint8_t device, uint8_t function,
                 uint8_t offset, uint16_t value) {
    pci_write_address(bus, device, function, offset);
    out16(kPciDataPort + (offset & 2u), value);
}

struct PciLocation {
    uint8_t bus;
    uint8_t device;
    uint8_t function;
    uint16_t io_base;
};

bool find_legacy_virtio_net(PciLocation* result) {
    if (result == nullptr) return false;
    for (uint32_t bus = 0; bus < 256; ++bus) {
        for (uint32_t device = 0; device < 32; ++device) {
            const uint32_t header = pci_read32(static_cast<uint8_t>(bus),
                static_cast<uint8_t>(device), 0, 0x0c);
            const uint8_t function_count = ((header >> 23) & 1u) != 0 ? 8 : 1;
            for (uint32_t function = 0; function < function_count; ++function) {
                const uint32_t id = pci_read32(static_cast<uint8_t>(bus),
                    static_cast<uint8_t>(device), static_cast<uint8_t>(function), 0);
                if (static_cast<uint16_t>(id) != kVirtioVendor ||
                    static_cast<uint16_t>(id >> 16) != kVirtioNetLegacyDevice) continue;

                const uint32_t bar0 = pci_read32(static_cast<uint8_t>(bus),
                    static_cast<uint8_t>(device), static_cast<uint8_t>(function), 0x10);
                if ((bar0 & 1u) == 0) continue;
                const uint32_t base = bar0 & ~3u;
                if (base == 0 || base > 0xffd0u) continue;

                result->bus = static_cast<uint8_t>(bus);
                result->device = static_cast<uint8_t>(device);
                result->function = static_cast<uint8_t>(function);
                result->io_base = static_cast<uint16_t>(base);
                return true;
            }
        }
    }
    return false;
}

bool configure_pci_command(const PciLocation& location) {
    const uint32_t command_status = pci_read32(location.bus, location.device,
                                                location.function, 0x04);
    uint16_t command = static_cast<uint16_t>(command_status);
    command |= kPciCommandIo | kPciCommandBusMaster;
    pci_write16(location.bus, location.device, location.function, 0x04, command);
    const uint16_t checked = static_cast<uint16_t>(pci_read32(
        location.bus, location.device, location.function, 0x04));
    return (checked & (kPciCommandIo | kPciCommandBusMaster)) ==
           (kPciCommandIo | kPciCommandBusMaster);
}

bool configure_queue(uint16_t queue_index, uint32_t queue_offset, QueueView* view) {
    if (view == nullptr) return false;
    out16(g_io_base + kQueueSelect, queue_index);
    const uint16_t maximum = in16(g_io_base + kQueueSize);
    if (maximum == 0 || maximum > kMaximumQueueLength) return false;

    const uint64_t physical = g_dma_physical + queue_offset;
    if ((physical & (kPageBytes - 1u)) != 0 || physical / kPageBytes > 0xffffffffull) {
        return false;
    }

    auto* base = g_dma_virtual + queue_offset;
    for (uint32_t index = 0; index < kQueueBytes; ++index) base[index] = 0;
    const uint32_t descriptor_bytes = sizeof(Descriptor) * maximum;
    const uint32_t available_bytes = sizeof(uint16_t) * (2u + maximum);
    const uint32_t used_offset = (descriptor_bytes + available_bytes + kPageBytes - 1u) &
                                 ~(kPageBytes - 1u);
    const uint32_t used_bytes = sizeof(uint16_t) * 2u + sizeof(UsedElement) * maximum;
    if (used_offset > kQueueBytes || used_bytes > kQueueBytes - used_offset) return false;

    view->descriptors = reinterpret_cast<Descriptor*>(base);
    view->available = reinterpret_cast<AvailableRing*>(base + descriptor_bytes);
    view->used = reinterpret_cast<UsedRing*>(base + used_offset);
    view->size = maximum;
    view->last_used_index = 0;

    out32(g_io_base + kGuestPageSize, kPageBytes);
    out32(g_io_base + kQueueAlign, kPageBytes);
    out32(g_io_base + kQueuePfn, static_cast<uint32_t>(physical / kPageBytes));
    return in32(g_io_base + kQueuePfn) == static_cast<uint32_t>(physical / kPageBytes);
}

void memory_barrier() { __asm__ volatile("" : : : "memory"); }

void return_rx_buffer(uint16_t descriptor_index) {
    g_rx_queue.available->ring[g_rx_queue.available->index % g_rx_queue.size] = descriptor_index;
    memory_barrier();
    g_rx_queue.available->index = static_cast<uint16_t>(g_rx_queue.available->index + 1);
    memory_barrier();
    out16(g_io_base + kQueueNotify, 0);
}

void reap_tx() {
    memory_barrier();
    if (g_tx_pending && g_tx_queue.last_used_index != g_tx_queue.used->index) {
        ++g_tx_queue.last_used_index;
        g_tx_pending = false;
    }
}

} // namespace

bool virtio_net_initialize(const BootInfo& boot_info) {
    g_ready = false;
    g_tx_pending = false;
    g_io_base = 0;
    g_mac = {};
    g_rx_queue = {};
    g_tx_queue = {};

    if (boot_info.dma_region == nullptr || boot_info.dma_region_size < kRequiredDmaBytes ||
        boot_info.dma_physical_address == 0 ||
        boot_info.dma_physical_address > 0xffffffffull - boot_info.dma_region_size ||
        (boot_info.dma_physical_address & (kPageBytes - 1u)) != 0 ||
        reinterpret_cast<uint64_t>(boot_info.dma_region) != boot_info.dma_physical_address) {
        return false;
    }

    PciLocation location{};
    if (!find_legacy_virtio_net(&location) || !configure_pci_command(location)) return false;
    g_io_base = location.io_base;
    g_dma_physical = boot_info.dma_physical_address;
    g_dma_virtual = static_cast<uint8_t*>(boot_info.dma_region);

    out8(g_io_base + kDeviceStatus, 0);
    if (in8(g_io_base + kDeviceStatus) != 0) return false;
    out8(g_io_base + kDeviceStatus, kStatusAcknowledge);
    out8(g_io_base + kDeviceStatus, kStatusAcknowledge | kStatusDriver);

    const uint32_t host_features = in32(g_io_base + kHostFeatures);
    if ((host_features & kFeatureMac) == 0) {
        out8(g_io_base + kDeviceStatus, kStatusAcknowledge | kStatusDriver | kStatusFailed);
        return false;
    }
    out32(g_io_base + kGuestFeatures, kFeatureMac);
    for (uint32_t index = 0; index < 6; ++index) {
        g_mac.bytes[index] = in8(g_io_base + kDeviceConfig + static_cast<uint16_t>(index));
    }

    if (!configure_queue(0, kRxQueueOffset, &g_rx_queue) ||
        !configure_queue(1, kTxQueueOffset, &g_tx_queue)) {
        out8(g_io_base + kDeviceStatus, kStatusAcknowledge | kStatusDriver | kStatusFailed);
        return false;
    }

    for (uint32_t index = 0; index < g_rx_queue.size; ++index) {
        const uint64_t buffer_physical = g_dma_physical + kRxBuffersOffset +
                                         index * kFrameBufferBytes;
        g_rx_queue.descriptors[index] = {
            buffer_physical, kFrameBufferBytes, 1u << 1, 0
        };
        g_rx_queue.available->ring[index] = static_cast<uint16_t>(index);
    }
    g_rx_queue.available->index = g_rx_queue.size;

    const uint64_t tx_buffer_physical = g_dma_physical + kTxBufferOffset;
    g_tx_queue.descriptors[0] = {tx_buffer_physical, 0, 0, 0};
    out8(g_io_base + kDeviceStatus, kStatusAcknowledge | kStatusDriver | kStatusDriverOk);
    if ((in8(g_io_base + kDeviceStatus) & kStatusDriverOk) == 0) return false;
    out16(g_io_base + kQueueSelect, 0);
    out16(g_io_base + kQueueNotify, 0);
    g_ready = true;
    return true;
}

bool virtio_net_ready() { return g_ready; }

MacAddress virtio_net_mac_address() { return g_mac; }

bool virtio_net_send(const uint8_t* frame, uint16_t length) {
    if (!g_ready || frame == nullptr || length < 14 || length > kMaximumEthernetFrameBytes) {
        return false;
    }
    reap_tx();
    if (g_tx_pending) return false;

    uint8_t* packet = g_dma_virtual + kTxBufferOffset;
    for (uint32_t index = 0; index < kNetHeaderBytes; ++index) packet[index] = 0;
    for (uint32_t index = 0; index < length; ++index) packet[kNetHeaderBytes + index] = frame[index];
    g_tx_queue.descriptors[0].length = kNetHeaderBytes + length;
    g_tx_queue.available->ring[g_tx_queue.available->index % g_tx_queue.size] = 0;
    memory_barrier();
    g_tx_queue.available->index = static_cast<uint16_t>(g_tx_queue.available->index + 1);
    memory_barrier();
    out16(g_io_base + kQueueNotify, 1);
    g_tx_pending = true;
    return true;
}

uint32_t virtio_net_poll(ReceiveFrame receive, void* context, uint32_t frame_budget) {
    if (!g_ready) return 0;
    reap_tx();
    uint32_t frames = 0;
    while (frames < frame_budget && g_rx_queue.last_used_index != g_rx_queue.used->index) {
        memory_barrier();
        const UsedElement element = g_rx_queue.used->ring[
            g_rx_queue.last_used_index % g_rx_queue.size];
        ++g_rx_queue.last_used_index;
        if (element.id < g_rx_queue.size && element.length >= kNetHeaderBytes &&
            element.length <= kFrameBufferBytes) {
            const uint16_t frame_length = static_cast<uint16_t>(element.length - kNetHeaderBytes);
            const uint8_t* frame = g_dma_virtual + kRxBuffersOffset +
                element.id * kFrameBufferBytes + kNetHeaderBytes;
            if (frame_length >= 14 && receive != nullptr) {
                receive(context, frame, frame_length);
                ++frames;
            }
            return_rx_buffer(static_cast<uint16_t>(element.id));
        }
    }
    return frames;
}

} // namespace annyos::network
