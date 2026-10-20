#ifndef VMM_H
#define VMM_H

#include <stdint.h>

#define VMM_PRESENT   (1ULL << 0)
#define VMM_WRITE     (1ULL << 1)
#define VMM_USER      (1ULL << 2)
#define VMM_PWT       (1ULL << 3)
#define VMM_PCD       (1ULL << 4)
#define VMM_GLOBAL    (1ULL << 8)
#define VMM_NO_EXEC   (1ULL << 63)

void vmm_init(void);

void vmm_map_page(uint64_t virt, uint64_t phys, uint64_t flags);
void vmm_unmap_page(uint64_t virt);
uint64_t vmm_translate(uint64_t virt);

void vmm_map_range(uint64_t virt, uint64_t phys, uint64_t size, uint64_t flags);

/*
 * Per-address-space API (ring 3 support).
 * A user address space shares the kernel identity mapping (PML4[0])
 * and owns an extra private range (e.g. PML4[1]).
 */
uint64_t *vmm_kernel_pml4(void);
uint64_t *vmm_new_address_space(void);       /* kernel entries cloned */
void vmm_activate(uint64_t *pml4);           /* switch CR3 */

void vmm_map_page_at(uint64_t *pml4, uint64_t virt, uint64_t phys, uint64_t flags);
void vmm_map_range_at(uint64_t *pml4, uint64_t virt, uint64_t phys, uint64_t size, uint64_t flags);

/* Replace leaf protection flags in [virt, virt+size) (keeps phys + present). */
void vmm_set_prot_at(uint64_t *pml4, uint64_t virt, uint64_t size, uint64_t flags);

/* Translate against a specific root table (physical CR3 value is fine). */
uint64_t vmm_translate_root(uint64_t root, uint64_t virt);
/* Translate against the *currently active* CR3. */
uint64_t vmm_translate_current(uint64_t virt);

/* Free every page (frames + tables) mapped under the private range
 * handled by the given PML4 (everything except the cloned kernel
 * entries). Frees the PML4 table itself. */
void vmm_free_address_space(uint64_t *pml4);

#endif
