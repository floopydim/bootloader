#include "vmm.h"
#include "pmm.h"
#include "panic.h"
#include <stddef.h>

#define PAGE_SIZE_4K   0x1000ULL
#define PAGE_SIZE_2M   0x200000ULL
#define ENTRIES_PER_PT 512ULL
#define VMM_LARGE      (1ULL << 7)
#define VMM_ADDR_MASK_4K (~0xFFFULL)
#define VMM_ADDR_MASK_2M (~0x1FFFFFULL)

static uint64_t *g_pml4 = 0;
static uint64_t *g_pdpt = 0;
static uint64_t *g_pd0  = 0;
static uint64_t *g_pd1  = 0;
static uint64_t *g_pd2  = 0;
static uint64_t *g_pd3  = 0;

static inline void write_cr3(uint64_t val) {
    __asm__ volatile("mov %0, %%cr3" :: "r"(val) : "memory");
}

static inline void invlpg(uint64_t addr) {
    __asm__ volatile("invlpg (%0)" :: "r"(addr) : "memory");
}

static uint64_t *alloc_table_zeroed(void) {
    uint64_t *tbl = (uint64_t*)pmm_alloc_page();
    if (!tbl) {
        panic("VMM: out of memory allocating page table");
    }

    for (uint64_t i = 0; i < ENTRIES_PER_PT; i++) {
        tbl[i] = 0;
    }

    return tbl;
}

static uint64_t table_entry_flags(uint64_t flags) {
    uint64_t out = VMM_PRESENT;

    if (flags & VMM_WRITE) out |= VMM_WRITE;
    if (flags & VMM_USER)  out |= VMM_USER;
    if (flags & VMM_PWT)   out |= VMM_PWT;
    if (flags & VMM_PCD)   out |= VMM_PCD;

    return out;
}

static uint64_t *entry_to_table(uint64_t entry) {
    return (uint64_t*)(uintptr_t)(entry & VMM_ADDR_MASK_4K);
}

static uint64_t *get_or_create_table(uint64_t *table, uint64_t index, uint64_t flags) {
    if (!(table[index] & VMM_PRESENT)) {
        uint64_t *new_table = alloc_table_zeroed();
        table[index] = ((uint64_t)(uintptr_t)new_table & VMM_ADDR_MASK_4K) | table_entry_flags(flags | VMM_WRITE);
    }

    return entry_to_table(table[index]);
}

static void split_large_pde(uint64_t *pd, uint64_t pd_index) {
    uint64_t entry = pd[pd_index];

    if (!(entry & VMM_PRESENT)) {
        panic("VMM: split_large_pde on non-present entry");
    }

    if (!(entry & VMM_LARGE)) {
        return;
    }

    uint64_t base  = entry & VMM_ADDR_MASK_2M;
    uint64_t flags = entry & ~VMM_ADDR_MASK_2M;
    uint64_t *pt   = alloc_table_zeroed();

    uint64_t pte_flags = flags & ~VMM_LARGE;

    for (uint64_t i = 0; i < ENTRIES_PER_PT; i++) {
        uint64_t phys = base + i * PAGE_SIZE_4K;
        pt[i] = (phys & VMM_ADDR_MASK_4K) | (pte_flags & ~VMM_LARGE);
    }

    pd[pd_index] = ((uint64_t)(uintptr_t)pt & VMM_ADDR_MASK_4K) | table_entry_flags(flags | VMM_WRITE);
}

static void fill_pd_2m_identity(uint64_t *pd, uint64_t base_addr) {
    if (!pd) {
        panic("VMM: null PD in bootstrap identity fill");
    }

    for (uint64_t i = 0; i < ENTRIES_PER_PT; i++) {
        uint64_t addr = base_addr + i * PAGE_SIZE_2M;
        pd[i] = (addr & VMM_ADDR_MASK_2M) | VMM_PRESENT | VMM_WRITE | VMM_LARGE;
    }
}

void vmm_init(void) {
    g_pml4 = alloc_table_zeroed();
    g_pdpt = alloc_table_zeroed();
    g_pd0  = alloc_table_zeroed();
    g_pd1  = alloc_table_zeroed();
    g_pd2  = alloc_table_zeroed();
    g_pd3  = alloc_table_zeroed();

    g_pml4[0] = ((uint64_t)(uintptr_t)g_pdpt & VMM_ADDR_MASK_4K) | VMM_PRESENT | VMM_WRITE;

    g_pdpt[0] = ((uint64_t)(uintptr_t)g_pd0 & VMM_ADDR_MASK_4K) | VMM_PRESENT | VMM_WRITE; // 0..1GB
    g_pdpt[1] = ((uint64_t)(uintptr_t)g_pd1 & VMM_ADDR_MASK_4K) | VMM_PRESENT | VMM_WRITE; // 1..2GB
    g_pdpt[2] = ((uint64_t)(uintptr_t)g_pd2 & VMM_ADDR_MASK_4K) | VMM_PRESENT | VMM_WRITE; // 2..3GB
    g_pdpt[3] = ((uint64_t)(uintptr_t)g_pd3 & VMM_ADDR_MASK_4K) | VMM_PRESENT | VMM_WRITE; // 3..4GB

    fill_pd_2m_identity(g_pd0, 0x00000000ULL);
    fill_pd_2m_identity(g_pd1, 0x40000000ULL);
    fill_pd_2m_identity(g_pd2, 0x80000000ULL);
    fill_pd_2m_identity(g_pd3, 0xC0000000ULL);

    write_cr3((uint64_t)(uintptr_t)g_pml4);
}

void vmm_map_page(uint64_t virt, uint64_t phys, uint64_t flags) {
    uint64_t pml4_i = (virt >> 39) & 0x1FFULL;
    uint64_t pdpt_i = (virt >> 30) & 0x1FFULL;
    uint64_t pd_i   = (virt >> 21) & 0x1FFULL;
    uint64_t pt_i   = (virt >> 12) & 0x1FFULL;

    uint64_t *pdpt = get_or_create_table(g_pml4, pml4_i, flags);
    uint64_t *pd   = get_or_create_table(pdpt, pdpt_i, flags);

    if ((pd[pd_i] & VMM_PRESENT) && (pd[pd_i] & VMM_LARGE)) {
        split_large_pde(pd, pd_i);
    }

    uint64_t *pt = get_or_create_table(pd, pd_i, flags);
    pt[pt_i] = (phys & VMM_ADDR_MASK_4K) | (flags | VMM_PRESENT);
    invlpg(virt);
}

void vmm_unmap_page(uint64_t virt) {
    uint64_t pml4_i = (virt >> 39) & 0x1FFULL;
    uint64_t pdpt_i = (virt >> 30) & 0x1FFULL;
    uint64_t pd_i   = (virt >> 21) & 0x1FFULL;
    uint64_t pt_i   = (virt >> 12) & 0x1FFULL;

    if (!(g_pml4[pml4_i] & VMM_PRESENT)) {
        return;
    }

    uint64_t *pdpt = entry_to_table(g_pml4[pml4_i]);
    if (!(pdpt[pdpt_i] & VMM_PRESENT)) {
        return;
    }

    uint64_t *pd = entry_to_table(pdpt[pdpt_i]);
    if (!(pd[pd_i] & VMM_PRESENT)) {
        return;
    }

    if (pd[pd_i] & VMM_LARGE) {
        split_large_pde(pd, pd_i);
    }

    uint64_t *pt = entry_to_table(pd[pd_i]);
    pt[pt_i] = 0;
    invlpg(virt);
}

uint64_t vmm_translate(uint64_t virt) {
    return vmm_translate_root((uint64_t)(uintptr_t)g_pml4, virt);
}

void vmm_map_range(uint64_t virt, uint64_t phys, uint64_t size, uint64_t flags) {
    if (size == 0) {
        return;
    }

    uint64_t end = virt + size;
    uint64_t cur_v = virt & VMM_ADDR_MASK_4K;
    uint64_t cur_p = phys & VMM_ADDR_MASK_4K;

    while (cur_v < end) {
        vmm_map_page(cur_v, cur_p, flags);
        cur_v += PAGE_SIZE_4K;
        cur_p += PAGE_SIZE_4K;
    }
}

/*
 * ================== per-address-space API ==================
 */

uint64_t *vmm_kernel_pml4(void) {
    return g_pml4;
}

uint64_t *vmm_new_address_space(void) {
    uint64_t *pml4 = alloc_table_zeroed();
    if (!pml4) {
        return NULL;
    }

    /* Clone the kernel side of the address space: the kernel identity
     * mapping stays available to interrupt handlers that run while a
     * user task is active, but pages remain supervisor-only so a ring 3
     * task cannot touch them. */
    for (uint64_t i = 0; i < ENTRIES_PER_PT; i++) {
        pml4[i] = g_pml4[i];
    }

    return pml4;
}

void vmm_activate(uint64_t *pml4) {
    write_cr3((uint64_t)(uintptr_t)pml4);
}

void vmm_map_page_at(uint64_t *pml4, uint64_t virt, uint64_t phys, uint64_t flags) {
    uint64_t pml4_i = (virt >> 39) & 0x1FFULL;
    uint64_t pdpt_i = (virt >> 30) & 0x1FFULL;
    uint64_t pd_i   = (virt >> 21) & 0x1FFULL;
    uint64_t pt_i   = (virt >> 12) & 0x1FFULL;

    uint64_t *pdpt = get_or_create_table(pml4, pml4_i, flags);
    uint64_t *pd   = get_or_create_table(pdpt, pdpt_i, flags);

    if ((pd[pd_i] & VMM_PRESENT) && (pd[pd_i] & VMM_LARGE)) {
        split_large_pde(pd, pd_i);
    }

    uint64_t *pt = get_or_create_table(pd, pd_i, flags);
    pt[pt_i] = (phys & VMM_ADDR_MASK_4K) | (flags | VMM_PRESENT);
    invlpg(virt);
}

void vmm_map_range_at(uint64_t *pml4, uint64_t virt, uint64_t phys, uint64_t size, uint64_t flags) {
    if (size == 0) {
        return;
    }

    uint64_t end = virt + size;
    uint64_t cur_v = virt & VMM_ADDR_MASK_4K;
    uint64_t cur_p = phys & VMM_ADDR_MASK_4K;

    while (cur_v < end) {
        vmm_map_page_at(pml4, cur_v, cur_p, flags);
        cur_v += PAGE_SIZE_4K;
        cur_p += PAGE_SIZE_4K;
    }
}

void vmm_set_prot_at(uint64_t *pml4, uint64_t virt, uint64_t size, uint64_t flags) {
    uint64_t end;
    uint64_t cur_v;

    if (!pml4 || size == 0) {
        return;
    }

    end = virt + size;
    cur_v = virt & VMM_ADDR_MASK_4K;

    while (cur_v < end) {
        uint64_t pml4_i = (cur_v >> 39) & 0x1FFULL;
        uint64_t pdpt_i = (cur_v >> 30) & 0x1FFULL;
        uint64_t pd_i   = (cur_v >> 21) & 0x1FFULL;
        uint64_t pt_i   = (cur_v >> 12) & 0x1FFULL;

        if (pml4[pml4_i] & VMM_PRESENT) {
            uint64_t *pdpt = entry_to_table(pml4[pml4_i]);
            if (pdpt[pdpt_i] & VMM_PRESENT) {
                uint64_t *pd = entry_to_table(pdpt[pdpt_i]);
                if (pd[pd_i] & VMM_PRESENT) {
                    if (pd[pd_i] & VMM_LARGE) {
                        split_large_pde(pd, pd_i);
                    }
                    uint64_t *pt = entry_to_table(pd[pd_i]);
                    if (pt[pt_i] & VMM_PRESENT) {
                        uint64_t phys = pt[pt_i] & VMM_ADDR_MASK_4K;
                        pt[pt_i] = (phys & VMM_ADDR_MASK_4K) | (flags | VMM_PRESENT);
                        invlpg(cur_v);
                    }
                }
            }
        }

        cur_v += PAGE_SIZE_4K;
    }
}

uint64_t vmm_translate_root(uint64_t root, uint64_t virt) {
    uint64_t *pml4 = entry_to_table(root);
    uint64_t pml4_i = (virt >> 39) & 0x1FFULL;
    uint64_t pdpt_i = (virt >> 30) & 0x1FFULL;
    uint64_t pd_i   = (virt >> 21) & 0x1FFULL;
    uint64_t pt_i   = (virt >> 12) & 0x1FFULL;

    if (!(pml4[pml4_i] & VMM_PRESENT)) {
        return 0;
    }

    uint64_t *pdpt = entry_to_table(pml4[pml4_i]);
    if (!(pdpt[pdpt_i] & VMM_PRESENT)) {
        return 0;
    }

    uint64_t *pd = entry_to_table(pdpt[pdpt_i]);
    if (!(pd[pd_i] & VMM_PRESENT)) {
        return 0;
    }

    if (pd[pd_i] & VMM_LARGE) {
        return (pd[pd_i] & VMM_ADDR_MASK_2M) | (virt & (PAGE_SIZE_2M - 1));
    }

    uint64_t *pt = entry_to_table(pd[pd_i]);
    if (!(pt[pt_i] & VMM_PRESENT)) {
        return 0;
    }

    return (pt[pt_i] & VMM_ADDR_MASK_4K) | (virt & (PAGE_SIZE_4K - 1));
}

uint64_t vmm_translate_current(uint64_t virt) {
    uint64_t cr3;

    __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));
    return vmm_translate_root(cr3, virt);
}

void vmm_free_address_space(uint64_t *pml4) {
    if (!pml4) {
        return;
    }

    /* Free every private top-level entry except the cloned kernel ones
     * (kernel entries are shared, never owned here). */
    for (uint64_t i = 0; i < ENTRIES_PER_PT; i++) {
        uint64_t entry = pml4[i];

        if (!(entry & VMM_PRESENT)) {
            continue;
        }
        if (!(entry & VMM_USER)) {
            continue;  /* kernel-cloned mapping (supervisor) */
        }

        uint64_t *pdpt = entry_to_table(entry);
        for (uint64_t j = 0; j < ENTRIES_PER_PT; j++) {
            uint64_t *pd;

            if (!(pdpt[j] & VMM_PRESENT)) {
                continue;
            }

            pd = entry_to_table(pdpt[j]);

            for (uint64_t k = 0; k < ENTRIES_PER_PT; k++) {
                uint64_t pd_entry = pd[k];

                if (!(pd_entry & VMM_PRESENT)) {
                    continue;
                }

                if (pd_entry & VMM_LARGE) {
                    pmm_free_page((void*)(uintptr_t)(pd_entry & VMM_ADDR_MASK_2M));
                    continue;
                }

                uint64_t *pt = entry_to_table(pd_entry);
                for (uint64_t k2 = 0; k2 < ENTRIES_PER_PT; k2++) {
                    if (pt[k2] & VMM_PRESENT) {
                        pmm_free_page((void*)(uintptr_t)(pt[k2] & VMM_ADDR_MASK_4K));
                    }
                }
                pmm_free_page((void*)pt);
                pd[k] = 0;
            }

            pmm_free_page((void*)pd);
            pdpt[j] = 0;
        }

        pmm_free_page((void*)pdpt);
        pml4[i] = 0;
    }

    pmm_free_page((void*)pml4);
}
