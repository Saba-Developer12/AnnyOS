#pragma once

#include <stdint.h>

#include "annyos/driver_services.hpp"

namespace annyos::network {

struct Ipv4Address {
    uint8_t bytes[4];
};

struct InterfaceConfig {
    Ipv4Address address;
    Ipv4Address subnet_mask;
    Ipv4Address gateway;
    Ipv4Address dns_server;
    bool configured;
};

// Starts DHCP on the supplied Ethernet frame service and processes bounded
// packet batches and lwIP timers when polled. The stack uses lwIP's NO_SYS raw
// API. Network clients must be integrated with this polling model.
bool network_stack_initialize(const annyos::drivers::service::EthernetFramesV1& ethernet);
uint32_t network_stack_poll(uint32_t frame_budget = 16);
InterfaceConfig network_interface_config();

} // namespace annyos::network
