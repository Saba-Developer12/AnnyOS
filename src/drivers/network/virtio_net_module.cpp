#include "annyos/virtio_net.hpp"

extern "C" bool annyos_virtio_net_initialize(const annyos::BootInfo* boot_info) {
    return boot_info != nullptr && annyos::network::virtio_net_initialize(*boot_info);
}

extern "C" bool annyos_virtio_net_ready() {
    return annyos::network::virtio_net_ready();
}

extern "C" annyos::network::MacAddress annyos_virtio_net_mac_address() {
    return annyos::network::virtio_net_mac_address();
}

extern "C" bool annyos_virtio_net_send(const uint8_t* frame, uint16_t length) {
    return annyos::network::virtio_net_send(frame, length);
}

extern "C" uint32_t annyos_virtio_net_poll(annyos::network::ReceiveFrame receive,
                                             void* context, uint32_t budget) {
    return annyos::network::virtio_net_poll(receive, context, budget);
}
