#include "annyos/network_stack.hpp"

#include "annyos/timer_pit.hpp"

#include "lwip/dhcp.h"
#include "lwip/dns.h"
#include "lwip/etharp.h"
#include "lwip/init.h"
#include "lwip/ip4.h"
#include "lwip/netif.h"
#include "lwip/pbuf.h"
#include "lwip/sys.h"
#include "lwip/timeouts.h"
#include "netif/ethernet.h"

namespace annyos::network {
namespace {

constexpr uint16_t kMaximumFrameBytes = 1518;

annyos::drivers::service::EthernetFramesV1 g_ethernet{};
MacAddress g_mac{};
InterfaceConfig g_config{};
struct netif g_netif{};
uint8_t g_frame[kMaximumFrameBytes];
bool g_initialized;

void copy_bytes(uint8_t* destination, const uint8_t* source, uint32_t length) {
    for (uint32_t index = 0; index < length; ++index) destination[index] = source[index];
}

err_t link_output(struct netif*, struct pbuf* packet) {
    if (packet == nullptr || packet->tot_len < 14 || packet->tot_len > sizeof(g_frame)) {
        return ERR_BUF;
    }
    const u16_t copied = pbuf_copy_partial(packet, g_frame, packet->tot_len, 0);
    if (copied != packet->tot_len) return ERR_BUF;
    return g_ethernet.send(g_frame, copied) ? ERR_OK : ERR_IF;
}

void ethernet_input_frame(void* context, const uint8_t* frame, uint16_t length) {
    auto* interface = static_cast<struct netif*>(context);
    if (interface == nullptr || frame == nullptr || length < 14 ||
        length > kMaximumFrameBytes) return;
    struct pbuf* packet = pbuf_alloc(PBUF_RAW, length, PBUF_POOL);
    if (packet == nullptr) return;
    err_t result = pbuf_take(packet, frame, length);
    if (result == ERR_OK) result = interface->input(packet, interface);
    if (result != ERR_OK) pbuf_free(packet);
}

err_t netif_initialize(struct netif* interface) {
    if (interface == nullptr) return ERR_ARG;
    interface->name[0] = 'e';
    interface->name[1] = 'n';
    interface->hwaddr_len = 6;
    copy_bytes(interface->hwaddr, g_mac.bytes, 6);
    interface->mtu = 1500;
    interface->flags = NETIF_FLAG_BROADCAST | NETIF_FLAG_ETHARP | NETIF_FLAG_ETHERNET;
    interface->output = etharp_output;
    interface->linkoutput = link_output;
    return ERR_OK;
}

void update_interface_config() {
    const ip4_addr_t* address = netif_ip4_addr(&g_netif);
    const ip4_addr_t* mask = netif_ip4_netmask(&g_netif);
    const ip4_addr_t* gateway = netif_ip4_gw(&g_netif);
    const ip_addr_t* dns = dns_getserver(0);
    g_config.address.bytes[0] = ip4_addr1(address);
    g_config.address.bytes[1] = ip4_addr2(address);
    g_config.address.bytes[2] = ip4_addr3(address);
    g_config.address.bytes[3] = ip4_addr4(address);
    g_config.subnet_mask.bytes[0] = ip4_addr1(mask);
    g_config.subnet_mask.bytes[1] = ip4_addr2(mask);
    g_config.subnet_mask.bytes[2] = ip4_addr3(mask);
    g_config.subnet_mask.bytes[3] = ip4_addr4(mask);
    g_config.gateway.bytes[0] = ip4_addr1(gateway);
    g_config.gateway.bytes[1] = ip4_addr2(gateway);
    g_config.gateway.bytes[2] = ip4_addr3(gateway);
    g_config.gateway.bytes[3] = ip4_addr4(gateway);
    if (dns != nullptr && IP_IS_V4(dns)) {
        const ip4_addr_t* dns4 = ip_2_ip4(dns);
        g_config.dns_server.bytes[0] = ip4_addr1(dns4);
        g_config.dns_server.bytes[1] = ip4_addr2(dns4);
        g_config.dns_server.bytes[2] = ip4_addr3(dns4);
        g_config.dns_server.bytes[3] = ip4_addr4(dns4);
    }
    g_config.configured = !ip4_addr_isany_val(*address);
}

} // namespace

bool network_stack_initialize(const annyos::drivers::service::EthernetFramesV1& ethernet) {
    g_initialized = false;
    g_config = {};
    if (ethernet.ready == nullptr || ethernet.mac_address == nullptr ||
        ethernet.send == nullptr || ethernet.poll == nullptr || !ethernet.ready()) return false;
    g_ethernet = ethernet;
    g_mac = ethernet.mac_address();

    lwip_init();
    ip4_addr_t zero_address;
    IP4_ADDR(&zero_address, 0, 0, 0, 0);
    if (netif_add(&g_netif, &zero_address, &zero_address, &zero_address,
                  nullptr, netif_initialize, ethernet_input) == nullptr) return false;
    netif_set_default(&g_netif);
    netif_set_up(&g_netif);
    netif_set_link_up(&g_netif);
    if (dhcp_start(&g_netif) != ERR_OK) {
        netif_set_down(&g_netif);
        netif_remove(&g_netif);
        return false;
    }
    g_initialized = true;
    return true;
}

uint32_t network_stack_poll(uint32_t frame_budget) {
    if (!g_initialized) return 0;
    const uint32_t received = g_ethernet.poll(ethernet_input_frame, &g_netif, frame_budget);
    sys_check_timeouts();
    update_interface_config();
    return received;
}

InterfaceConfig network_interface_config() {
    if (g_initialized) update_interface_config();
    return g_config;
}


} // namespace annyos::network

extern "C" uint32_t sys_now(void) {
    return static_cast<uint32_t>(annyos::timer::ticks());
}

extern "C" uint32_t annyos_lwip_rand(void) {
    annyos::network::InterfaceConfig config = annyos::network::network_interface_config();
    uint32_t seed = 0;
    for (uint32_t index = 0; index < 4; ++index) seed = (seed << 8) | config.address.bytes[index];
    static uint32_t state = 0x414e4e59u;
    state ^= seed + static_cast<uint32_t>(annyos::timer::ticks());
    state ^= state << 13;
    state ^= state >> 17;
    state ^= state << 5;
    return state;
}

extern "C" void annyos_lwip_assert(const char*, const char*, int) {
    __asm__ volatile("outb %0, $0xe9" : : "a"(static_cast<uint8_t>('!')));
    for (;;) __asm__ volatile("cli; hlt" : : : "memory");
}
