#include <stdbool.h>
#include <stdint.h>

#include <kos/cpu.h>
#include <kos/memory.h>
#include <kos/pmm.h>
#include <kos/sync.h>
#include <kos/vmm.h>

enum {
    PAGE_SIZE = KOS_PAGE_SIZE,
    PAGE_PRESENT = 1ull << 0,
    PAGE_HUGE = 1ull << 7,
    PAGE_KOS_OWNED_TABLE = 1ull << 9,
    PAGE_KOS_OWNED_MAPPING = 1ull << 10,
    PAGE_ADDRESS_MASK = 0x000ffffffffff000ull,
    PAGE_2M_ADDRESS_MASK = 0x000fffffffe00000ull,
    PAGE_1G_ADDRESS_MASK = 0x000fffffc0000000ull,
};

struct virtual_memory_manager {
    uint64_t hhdm_offset;
    uint64_t *pml4;
    bool initialized;
};

static struct virtual_memory_manager vmm;
static struct kos_spinlock vmm_lock = KOS_SPINLOCK_INITIALIZER;

static uint64_t *physical_to_virtual(uint64_t physical_address) {
    return (uint64_t *)(vmm.hhdm_offset + physical_address);
}

static uint64_t page_table_index(uint64_t virtual_address, uint64_t shift) {
    return (virtual_address >> shift) & 0x1ff;
}

static void clear_page_table(uint64_t *table) {
    for (uint64_t index = 0; index < 512; ++index) {
        table[index] = 0;
    }
}

static bool page_table_is_empty(const uint64_t *table) {
    for (uint64_t index = 0; index < 512; ++index) {
        if ((table[index] & PAGE_PRESENT) != 0) {
            return false;
        }
    }
    return true;
}

static bool release_owned_empty_table(uint64_t *entry) {
    uint64_t value = *entry;
    if ((value & (PAGE_PRESENT | PAGE_KOS_OWNED_TABLE))
            != (PAGE_PRESENT | PAGE_KOS_OWNED_TABLE)) {
        return false;
    }
    uint64_t *table = physical_to_virtual(value & PAGE_ADDRESS_MASK);
    if (!page_table_is_empty(table)) {
        return false;
    }
    *entry = 0;
    return pmm_free_frame(value & PAGE_ADDRESS_MASK);
}

static uint64_t public_page_flags(uint64_t flags) {
    return flags & ~(PAGE_KOS_OWNED_TABLE | PAGE_KOS_OWNED_MAPPING);
}

static uint64_t combine_effective_flags(uint64_t parent_flags, uint64_t entry) {
    uint64_t result = entry;
    if ((parent_flags & VMM_PAGE_WRITABLE) == 0) {
        result &= ~VMM_PAGE_WRITABLE;
    }
    if ((parent_flags & VMM_PAGE_NO_EXECUTE) != 0) {
        result |= VMM_PAGE_NO_EXECUTE;
    }
    return result;
}

static uint64_t *next_table(uint64_t *entry, bool *created) {
    *created = false;
    if ((*entry & PAGE_PRESENT) != 0) {
        if ((*entry & PAGE_HUGE) != 0) {
            return 0;
        }
        return physical_to_virtual(*entry & PAGE_ADDRESS_MASK);
    }

    uint64_t frame = pmm_allocate_frame();
    if (frame == 0) {
        return 0;
    }
    uint64_t *table = physical_to_virtual(frame);
    clear_page_table(table);
    *entry = frame | PAGE_PRESENT | VMM_PAGE_WRITABLE | PAGE_KOS_OWNED_TABLE;
    *created = true;
    return table;
}

bool vmm_initialize(uint64_t hhdm_offset) {
    uint64_t cr3 = cpu_read_cr3() & PAGE_ADDRESS_MASK;
    if (hhdm_offset == 0 || hhdm_offset % PAGE_SIZE != 0 || cr3 == 0) {
        return false;
    }
    uint64_t flags = spinlock_lock_irqsave(&vmm_lock);
    vmm.hhdm_offset = hhdm_offset;
    vmm.pml4 = physical_to_virtual(cr3);
    vmm.initialized = true;
    spinlock_unlock_irqrestore(&vmm_lock, flags);
    return true;
}

bool vmm_map_page(uint64_t virtual_address, uint64_t physical_address, uint64_t flags) {
    if (virtual_address % PAGE_SIZE != 0 || physical_address % PAGE_SIZE != 0
        || (flags & ~(VMM_PAGE_WRITABLE | VMM_PAGE_NO_EXECUTE)) != 0) {
        return false;
    }
    uint64_t interrupt_flags = spinlock_lock_irqsave(&vmm_lock);
    if (!vmm.initialized) {
        spinlock_unlock_irqrestore(&vmm_lock, interrupt_flags);
        return false;
    }

    uint64_t *pml4_entry = &vmm.pml4[page_table_index(virtual_address, 39)];
    bool pml4_table_created;
    uint64_t *pdpt = next_table(pml4_entry, &pml4_table_created);
    if (pdpt == 0) {
        spinlock_unlock_irqrestore(&vmm_lock, interrupt_flags);
        return false;
    }
    uint64_t *pdpt_entry = &pdpt[page_table_index(virtual_address, 30)];
    bool pdpt_table_created;
    uint64_t *directory = next_table(pdpt_entry, &pdpt_table_created);
    if (directory == 0) {
        goto rollback_pml4;
    }
    uint64_t *directory_entry = &directory[page_table_index(virtual_address, 21)];
    bool directory_table_created;
    uint64_t *table = next_table(directory_entry, &directory_table_created);
    if (table == 0) {
        goto rollback_pdpt;
    }
    uint64_t *entry = &table[page_table_index(virtual_address, 12)];
    if ((*entry & PAGE_PRESENT) != 0) {
        goto rollback_directory;
    }

    *entry = (physical_address & PAGE_ADDRESS_MASK) | PAGE_PRESENT | PAGE_KOS_OWNED_MAPPING | flags;
    cpu_invalidate_page(virtual_address);
    spinlock_unlock_irqrestore(&vmm_lock, interrupt_flags);
    return true;

rollback_directory:
    if (directory_table_created) {
        release_owned_empty_table(directory_entry);
    }
rollback_pdpt:
    if (pdpt_table_created) {
        release_owned_empty_table(pdpt_entry);
    }
rollback_pml4:
    if (pml4_table_created) {
        release_owned_empty_table(pml4_entry);
    }
    spinlock_unlock_irqrestore(&vmm_lock, interrupt_flags);
    return false;
}

static bool vmm_query_page_unlocked(uint64_t virtual_address, uint64_t *physical_address, uint64_t *flags) {
    if (!vmm.initialized) {
        return false;
    }
    uint64_t entry = vmm.pml4[page_table_index(virtual_address, 39)];
    if ((entry & PAGE_PRESENT) == 0) {
        return false;
    }
    uint64_t effective_flags = entry;
    uint64_t *pdpt = physical_to_virtual(entry & PAGE_ADDRESS_MASK);
    entry = pdpt[page_table_index(virtual_address, 30)];
    if ((entry & PAGE_PRESENT) == 0) {
        return false;
    }
    effective_flags = combine_effective_flags(effective_flags, entry);
    if ((entry & PAGE_HUGE) != 0) {
        if (physical_address != 0) {
            *physical_address = (entry & PAGE_1G_ADDRESS_MASK) | (virtual_address & ((1ull << 30) - 1));
        }
        if (flags != 0) {
            *flags = public_page_flags(effective_flags);
        }
        return true;
    }
    uint64_t *directory = physical_to_virtual(entry & PAGE_ADDRESS_MASK);
    entry = directory[page_table_index(virtual_address, 21)];
    if ((entry & PAGE_PRESENT) == 0) {
        return false;
    }
    effective_flags = combine_effective_flags(effective_flags, entry);
    if ((entry & PAGE_HUGE) != 0) {
        if (physical_address != 0) {
            *physical_address = (entry & PAGE_2M_ADDRESS_MASK) | (virtual_address & ((1ull << 21) - 1));
        }
        if (flags != 0) {
            *flags = public_page_flags(effective_flags);
        }
        return true;
    }
    uint64_t *table = physical_to_virtual(entry & PAGE_ADDRESS_MASK);
    entry = table[page_table_index(virtual_address, 12)];
    if ((entry & PAGE_PRESENT) == 0) {
        return false;
    }
    effective_flags = combine_effective_flags(effective_flags, entry);
    if (physical_address != 0) {
        *physical_address = (entry & PAGE_ADDRESS_MASK) | (virtual_address & (PAGE_SIZE - 1));
    }
    if (flags != 0) {
        *flags = public_page_flags(effective_flags);
    }
    return true;
}

bool vmm_query_page(uint64_t virtual_address, uint64_t *physical_address, uint64_t *flags) {
    uint64_t interrupt_flags = spinlock_lock_irqsave(&vmm_lock);
    bool result = vmm_query_page_unlocked(virtual_address, physical_address, flags);
    spinlock_unlock_irqrestore(&vmm_lock, interrupt_flags);
    return result;
}

bool vmm_is_mapped(uint64_t virtual_address) {
    return vmm_query_page(virtual_address, 0, 0);
}

static bool set_leaf_permissions(uint64_t virtual_address, uint64_t *entry, uint64_t permissions) {
    *entry = (*entry & ~(VMM_PAGE_WRITABLE | VMM_PAGE_NO_EXECUTE)) | permissions;
    cpu_invalidate_page(virtual_address);
    return true;
}

static bool vmm_set_page_permissions_unlocked(uint64_t virtual_address, uint64_t permissions) {
    if (!vmm.initialized || (permissions & ~(VMM_PAGE_WRITABLE | VMM_PAGE_NO_EXECUTE)) != 0) {
        return false;
    }

    uint64_t *pml4_entry = &vmm.pml4[page_table_index(virtual_address, 39)];
    uint64_t pml4_value = *pml4_entry;
    if ((pml4_value & PAGE_PRESENT) == 0 || (pml4_value & PAGE_HUGE) != 0) {
        return false;
    }
    uint64_t *pdpt = physical_to_virtual(pml4_value & PAGE_ADDRESS_MASK);
    uint64_t *pdpt_entry = &pdpt[page_table_index(virtual_address, 30)];
    uint64_t pdpt_value = *pdpt_entry;
    if ((pdpt_value & PAGE_PRESENT) == 0) {
        return false;
    }
    if ((pdpt_value & PAGE_HUGE) != 0) {
        return set_leaf_permissions(virtual_address, pdpt_entry, permissions);
    }
    uint64_t *directory = physical_to_virtual(pdpt_value & PAGE_ADDRESS_MASK);
    uint64_t *directory_entry = &directory[page_table_index(virtual_address, 21)];
    uint64_t directory_value = *directory_entry;
    if ((directory_value & PAGE_PRESENT) == 0) {
        return false;
    }
    if ((directory_value & PAGE_HUGE) != 0) {
        return set_leaf_permissions(virtual_address, directory_entry, permissions);
    }
    uint64_t *table = physical_to_virtual(directory_value & PAGE_ADDRESS_MASK);
    uint64_t *page_entry = &table[page_table_index(virtual_address, 12)];
    if ((*page_entry & PAGE_PRESENT) == 0) {
        return false;
    }
    return set_leaf_permissions(virtual_address, page_entry, permissions);
}

bool vmm_set_page_permissions(uint64_t virtual_address, uint64_t permissions) {
    uint64_t interrupt_flags = spinlock_lock_irqsave(&vmm_lock);
    bool result = vmm_set_page_permissions_unlocked(virtual_address, permissions);
    spinlock_unlock_irqrestore(&vmm_lock, interrupt_flags);
    return result;
}

static uint64_t vmm_unmap_page_unlocked(uint64_t virtual_address) {
    if (!vmm.initialized || virtual_address % PAGE_SIZE != 0) {
        return 0;
    }

    uint64_t *pml4_entry = &vmm.pml4[page_table_index(virtual_address, 39)];
    uint64_t pml4_value = *pml4_entry;
    if ((pml4_value & PAGE_PRESENT) == 0 || (pml4_value & PAGE_HUGE) != 0) {
        return 0;
    }
    uint64_t *pdpt = physical_to_virtual(pml4_value & PAGE_ADDRESS_MASK);
    uint64_t *pdpt_entry = &pdpt[page_table_index(virtual_address, 30)];
    uint64_t pdpt_value = *pdpt_entry;
    if ((pdpt_value & PAGE_PRESENT) == 0 || (pdpt_value & PAGE_HUGE) != 0) {
        return 0;
    }
    uint64_t *directory = physical_to_virtual(pdpt_value & PAGE_ADDRESS_MASK);
    uint64_t *directory_entry = &directory[page_table_index(virtual_address, 21)];
    uint64_t directory_value = *directory_entry;
    if ((directory_value & PAGE_PRESENT) == 0 || (directory_value & PAGE_HUGE) != 0) {
        return 0;
    }
    uint64_t *table = physical_to_virtual(directory_value & PAGE_ADDRESS_MASK);
    uint64_t *page_entry = &table[page_table_index(virtual_address, 12)];
    uint64_t entry = *page_entry;
    if ((entry & (PAGE_PRESENT | PAGE_KOS_OWNED_MAPPING))
            != (PAGE_PRESENT | PAGE_KOS_OWNED_MAPPING)) {
        return 0;
    }

    *page_entry = 0;
    cpu_invalidate_page(virtual_address);
    uint64_t physical_address = entry & PAGE_ADDRESS_MASK;

    if (release_owned_empty_table(directory_entry)) {
        if (release_owned_empty_table(pdpt_entry)) {
            release_owned_empty_table(pml4_entry);
        }
    }
    return physical_address;
}

uint64_t vmm_unmap_page(uint64_t virtual_address) {
    uint64_t interrupt_flags = spinlock_lock_irqsave(&vmm_lock);
    uint64_t physical_address = vmm_unmap_page_unlocked(virtual_address);
    spinlock_unlock_irqrestore(&vmm_lock, interrupt_flags);
    return physical_address;
}

bool vmm_map_pages(uint64_t virtual_address, uint64_t physical_address, uint64_t page_count, uint64_t flags) {
    if (page_count == 0) {
        return true;
    }
    for (uint64_t i = 0; i < page_count; ++i) {
        if (!vmm_map_page(virtual_address + i * PAGE_SIZE, physical_address + i * PAGE_SIZE, flags)) {
            for (uint64_t j = 0; j < i; ++j) {
                (void)vmm_unmap_page(virtual_address + j * PAGE_SIZE);
            }
            return false;
        }
    }
    return true;
}

uint64_t vmm_unmap_pages(uint64_t virtual_address, uint64_t page_count) {
    uint64_t unmapped = 0;
    for (uint64_t i = 0; i < page_count; ++i) {
        if (vmm_unmap_page(virtual_address + i * PAGE_SIZE) != 0) {
            unmapped++;
        }
    }
    return unmapped;
}
