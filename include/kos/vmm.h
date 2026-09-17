#ifndef KOS_VMM_H
#define KOS_VMM_H

#include <stdbool.h>
#include <stdint.h>

enum {
    VMM_PAGE_WRITABLE = 1ull << 1,
    VMM_PAGE_NO_EXECUTE = 1ull << 63,
};

bool vmm_initialize(uint64_t hhdm_offset);
bool vmm_map_page(uint64_t virtual_address, uint64_t physical_address, uint64_t flags);
uint64_t vmm_unmap_page(uint64_t virtual_address);
bool vmm_set_page_permissions(uint64_t virtual_address, uint64_t permissions);
bool vmm_is_mapped(uint64_t virtual_address);
bool vmm_query_page(uint64_t virtual_address, uint64_t *physical_address, uint64_t *flags);
bool vmm_map_pages(uint64_t virtual_address, uint64_t physical_address, uint64_t page_count, uint64_t flags);
uint64_t vmm_unmap_pages(uint64_t virtual_address, uint64_t page_count);

#endif
