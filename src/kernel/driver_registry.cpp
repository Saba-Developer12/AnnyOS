#include "annyos/driver_registry.hpp"

namespace annyos::drivers {
namespace {

bool valid_descriptor(const DriverDescriptor& descriptor) {
    if (descriptor.name == nullptr || descriptor.name[0] == '\0' ||
        descriptor.version == 0 || descriptor.initialize == nullptr ||
        (descriptor.requirement_count != 0 && descriptor.requirements == nullptr)) {
        return false;
    }
    for (uint32_t i = 0; i < descriptor.requirement_count; ++i) {
        if (descriptor.requirements[i].minimum_version == 0) return false;
        for (uint32_t j = 0; j < i; ++j) {
            if (descriptor.requirements[i].id == descriptor.requirements[j].id) return false;
        }
    }
    return true;
}

bool dependencies_ready(const DriverRegistry& registry, const DriverDescriptor& descriptor) {
    for (uint32_t i = 0; i < descriptor.requirement_count; ++i) {
        if (registry.query(descriptor.requirements[i].id,
                           descriptor.requirements[i].minimum_version) == nullptr) {
            return false;
        }
    }
    return true;
}

} // namespace

void DriverRegistry::remove_services_from(DriverId provider) {
    uint32_t destination = 0;
    for (uint32_t i = 0; i < service_count_; ++i) {
        if (services_[i].provider == provider) continue;
        if (destination != i) services_[destination] = services_[i];
        ++destination;
    }
    service_count_ = destination;
}

void DriverRegistry::reset() {
    driver_count_ = 0;
    service_count_ = 0;
    has_starting_driver_ = false;
    initialization_started_ = false;
}

bool DriverRegistry::register_driver(const DriverDescriptor& descriptor) {
    if (initialization_started_ || !valid_descriptor(descriptor) ||
        driver_count_ >= kMaximumDrivers ||
        find(descriptor.id) != nullptr) {
        return false;
    }
    drivers_[driver_count_++] = {&descriptor, DriverState::registered};
    return true;
}

bool DriverRegistry::publish(ServiceId id, uint16_t version, const void* interface_pointer) {
    if (!has_starting_driver_ || version == 0 || interface_pointer == nullptr) return false;
    for (uint32_t i = 0; i < service_count_; ++i) {
        if (services_[i].id == id) return false;
    }
    if (service_count_ >= kMaximumServices) return false;
    services_[service_count_++] = {id, version, starting_driver_, interface_pointer};
    return true;
}

const ServiceRecord* DriverRegistry::query(ServiceId id, uint16_t minimum_version) const {
    for (uint32_t i = 0; i < service_count_; ++i) {
        if (services_[i].id == id && services_[i].version >= minimum_version) {
            return &services_[i];
        }
    }
    return nullptr;
}

const DriverRecord* DriverRegistry::find(DriverId id) const {
    for (uint32_t i = 0; i < driver_count_; ++i) {
        if (drivers_[i].descriptor->id == id) return &drivers_[i];
    }
    return nullptr;
}

InitializationSummary DriverRegistry::initialize_all(void* kernel_context) {
    InitializationSummary summary{};
    if (initialization_started_) return summary;
    initialization_started_ = true;
    uint32_t pending = 0;
    for (uint32_t i = 0; i < driver_count_; ++i) {
        if (drivers_[i].state == DriverState::registered) ++pending;
    }

    while (pending != 0) {
        bool made_progress = false;
        for (uint32_t i = 0; i < driver_count_; ++i) {
            DriverRecord& record = drivers_[i];
            if (record.state != DriverState::registered ||
                !dependencies_ready(*this, *record.descriptor)) {
                continue;
            }

            record.state = DriverState::starting;
            starting_driver_ = record.descriptor->id;
            has_starting_driver_ = true;
            const bool initialized = record.descriptor->initialize(kernel_context, *this);
            has_starting_driver_ = false;
            --pending;
            made_progress = true;
            if (initialized) {
                record.state = DriverState::started;
                ++summary.started;
            } else {
                remove_services_from(record.descriptor->id);
                record.state = DriverState::failed;
                ++summary.failed;
            }
        }
        if (!made_progress) break;
    }

    for (uint32_t i = 0; i < driver_count_; ++i) {
        if (drivers_[i].state == DriverState::registered) {
            drivers_[i].state = DriverState::blocked;
            ++summary.blocked;
        }
    }
    return summary;
}

} // namespace annyos::drivers
