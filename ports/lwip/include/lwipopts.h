#pragma once

// Single-threaded lwIP configuration for AnnyOS's polled kernel network loop.
#define NO_SYS 1
#define SYS_LIGHTWEIGHT_PROT 0
#define LWIP_SOCKET 0
#define LWIP_NETCONN 0
#define LWIP_NETIF_API 0
#define LWIP_TCPIP_CORE_LOCKING 0

#define LWIP_IPV4 1
#define LWIP_IPV6 0
#define LWIP_ARP 1
#define LWIP_ETHERNET 1
#define LWIP_ICMP 1
#define LWIP_UDP 1
#define LWIP_TCP 1
#define LWIP_RAW 0
#define LWIP_DHCP 1
#define LWIP_DNS 1
#define LWIP_AUTOIP 0
#define LWIP_IGMP 0
#define LWIP_DHCP_MAX_DNS_SERVERS 1
#define DNS_MAX_SERVERS 1
#define LWIP_DNS_SECURE 0

#define LWIP_ALTCP 1
#define LWIP_ALTCP_TLS 0
#define LWIP_HTTP_CLIENT 1
#define LWIP_HTTPC_HAVE_FILE_IO 0

#define MEM_ALIGNMENT 8
#define MEM_SIZE (64 * 1024)
#define MEMP_NUM_PBUF 24
#define MEMP_NUM_TCP_PCB 4
#define MEMP_NUM_TCP_PCB_LISTEN 1
#define MEMP_NUM_TCP_SEG 24
#define MEMP_NUM_SYS_TIMEOUT 16
#define PBUF_POOL_SIZE 24
#define PBUF_POOL_BUFSIZE 1600

#define TCP_MSS 1460
#define TCP_WND (2 * TCP_MSS)
#define TCP_SND_BUF (2 * TCP_MSS)
#define TCP_SND_QUEUELEN 8
#define LWIP_NETIF_TX_SINGLE_PBUF 1
#define LWIP_NETIF_LINK_CALLBACK 1
#define LWIP_NETIF_HOSTNAME 1

#define LWIP_CHECKSUM_CTRL_PER_NETIF 0
#define LWIP_STATS 0
#define LWIP_DEBUG 0
#define LWIP_TIMERS 1
