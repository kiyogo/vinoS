#ifndef KOS_UAPI_SYSTEM_INFO_H
#define KOS_UAPI_SYSTEM_INFO_H

#include <stdint.h>

#define KOS_SYSTEM_INFO_ABI_VERSION 2u
#define KOS_SYSTEM_INFO_DEVICE_NAME_MAX 32u

struct kos_system_info {
    uint32_t abi_version;
    uint32_t task_count;
    uint64_t uptime_seconds;
    /* Bytes reported by the physical-memory manager. */
    uint64_t total_memory_bytes;
    uint64_t free_memory_bytes;
    uint32_t network_available;
    uint32_t configuration_source;
    uint32_t dhcp_state;
    uint32_t ipv4_address;
    uint32_t subnet_mask;
    uint32_t gateway;
    uint32_t dns_server;
    uint8_t mac_address[6];
    char system_volume_letter;
    char device_name[KOS_SYSTEM_INFO_DEVICE_NAME_MAX];
};

#endif /* KOS_UAPI_SYSTEM_INFO_H */
