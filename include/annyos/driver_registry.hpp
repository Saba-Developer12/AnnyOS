#pragma once

#include <stdint.h>

namespace annyos::drivers {

constexpr uint32_t kMaximumDrivers = 128;
constexpr uint32_t kMaximumServices = 128;

enum class DriverState : uint8_t {
    registered,
    starting,
    started,
    failed,
    blocked,
};

enum class ServiceId : uint32_t {
    serial_output = 1,
    monotonic_timer = 2,
    keyboard_input = 3,
    ata_devices = 4,
    fat32_volumes = 5,
    ethernet_frames = 6,
};

enum class DriverId : uint32_t {
    serial_com1 = 1,
    timer_pit = 2,
    ps2_keyboard = 3,
    ata_pio = 4,
    fat32 = 5,
    virtio_net = 6,
};

struct ServiceRequirement {
    ServiceId id;
    uint16_t minimum_version;
};

class DriverRegistry;
using DriverInitialize = bool (*)(void* kernel_context, DriverRegistry& registry);

struct DriverDescriptor {
    DriverId id;
    const char* name;
    uint16_t version;
    const ServiceRequirement* requirements;
    uint8_t requirement_count;
    DriverInitialize initialize;
};

struct DriverRecord {
    const DriverDescriptor* descriptor;
    DriverState state;
};

struct ServiceRecord {
    ServiceId id;
    uint16_t version;
    DriverId provider;
    const void* interface_pointer;
};

struct InitializationSummary {
    uint32_t started;
    uint32_t failed;
    uint32_t blocked;
};

// Fixed-capacity registry for the current single-core, no-heap kernel. Drivers
// declare versioned service dependencies and publish interfaces at startup.
class DriverRegistry {
public:
    constexpr DriverRegistry()
        : drivers_{}, services_{}, driver_count_(0), service_count_(0),
          starting_driver_{}, has_starting_driver_(false), initialization_started_(false) {}

    void reset();
    bool register_driver(const DriverDescriptor& descriptor);
    InitializationSummary initialize_all(void* kernel_context);
    bool publish(ServiceId id, uint16_t version, const void* interface_pointer);
    const ServiceRecord* query(ServiceId id, uint16_t minimum_version = 1) const;
    const DriverRecord* find(DriverId id) const;

private:
    DriverRecord drivers_[kMaximumDrivers];
    ServiceRecord services_[kMaximumServices];
    uint32_t driver_count_;
    uint32_t service_count_;
    DriverId starting_driver_;
    bool has_starting_driver_;
    bool initialization_started_;

    void remove_services_from(DriverId provider);
};

} // namespace annyos::drivers
