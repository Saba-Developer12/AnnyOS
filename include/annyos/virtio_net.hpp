#pragma once

#include <stdint.h>

#include "annyos/boot_info.hpp"

namespace annyos::network {

struct MacAddress {
    uint8_t bytes[6];
};

using ReceiveFrame = void (*)(void* context, const uint8_t* frame, uint16_t length);

// Polling driver for the legacy PCI virtio-net interface (PCI ID 1AF4:1000).
// DMA is restricted to the low arena prepared by the UEFI loader.
bool virtio_net_initialize(const BootInfo& boot_info);
bool virtio_net_ready();
MacAddress virtio_net_mac_address();
bool virtio_net_send(const uint8_t* frame, uint16_t length);
uint32_t virtio_net_poll(ReceiveFrame receive, void* context,
                         uint32_t frame_budget = 16);

} // namespace annyos::network
