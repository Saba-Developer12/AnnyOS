#include "annyos/e1000.hpp"

extern "C" bool annyos_e1000_initialize(const annyos::BootInfo* boot_info) {
    return boot_info != nullptr && annyos::network::e1000_initialize(*boot_info);
}

extern "C" bool annyos_e1000_ready() {
    return annyos::network::e1000_ready();
}

extern "C" annyos::network::MacAddress annyos_e1000_mac_address() {
    return annyos::network::e1000_mac_address();
}

extern "C" bool annyos_e1000_send(const uint8_t* frame, uint16_t length) {
    return annyos::network::e1000_send(frame, length);
}

extern "C" uint32_t annyos_e1000_poll(annyos::network::ReceiveFrame receive,
                                        void* context, uint32_t budget) {
    return annyos::network::e1000_poll(receive, context, budget);
}
