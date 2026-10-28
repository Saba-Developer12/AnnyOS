#pragma once

#include <stdint.h>

#include "annyos/boot_info.hpp"
#include "annyos/virtio_net.hpp"

namespace annyos::network {

// Polling driver for the Intel 82540EM / QEMU e1000 PCI device (8086:100E).
// DMA memory comes from the low, identity-mapped UEFI arena.
bool e1000_initialize(const BootInfo& boot_info);
bool e1000_ready();
MacAddress e1000_mac_address();
bool e1000_send(const uint8_t* frame, uint16_t length);
uint32_t e1000_poll(ReceiveFrame receive, void* context, uint32_t frame_budget = 16);

} // namespace annyos::network
