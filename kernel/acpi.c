#include "acpi.h"
#include "io.h"
#include "panic.h"
#include <stddef.h>

static int acpi_initialized = 0;
static acpi_rsdp2_t *rsdp = NULL;
static acpi_rsdt_t *rsdt = NULL;
static acpi_xsdt_t *xsdt = NULL;
static int use_xsdt = 0;

/* Helper: memory compare */
static int acpi_memcmp(const void *a, const void *b, uint64_t size) {
    const uint8_t *pa = (const uint8_t*)a;
    const uint8_t *pb = (const uint8_t*)b;
    for (uint64_t i = 0; i < size; i++) {
        if (pa[i] != pb[i]) {
            return 1;
        }
    }
    return 0;
}

/* Helper: calculate checksum */
static uint8_t acpi_checksum(const void *data, uint64_t length) {
    const uint8_t *bytes = (const uint8_t*)data;
    uint8_t sum = 0;
    
    for (uint64_t i = 0; i < length; i++) {
        sum += bytes[i];
    }
    
    return sum;
}

/* Validate RSDP */
static int acpi_validate_rsdp(acpi_rsdp_t *r) {
    if (!r) {
        return 0;
    }
    
    if (acpi_memcmp(r->signature, ACPI_SIG_RSDP, 8) != 0) {
        return 0;
    }
    
    if (acpi_checksum(r, 20) != 0) {
        return 0;
    }
    
    return 1;
}

/* Validate RSDP 2.0 */
static int acpi_validate_rsdp2(acpi_rsdp2_t *r) {
    if (!acpi_validate_rsdp(&r->rsdp1)) {
        return 0;
    }
    
    if (r->rsdp1.revision < 2) {
        return 0;
    }
    
    if (acpi_checksum(r, r->length) != 0) {
        return 0;
    }
    
    return 1;
}

/* Validate SDT header */
static int acpi_validate_sdt(acpi_sdt_header_t *header) {
    if (!header) {
        return 0;
    }
    
    if (acpi_checksum(header, header->length) != 0) {
        return 0;
    }
    
    return 1;
}

void acpi_init(uint64_t rsdp_address) {
    acpi_rsdp_t *r1;
    acpi_rsdp2_t *r2;
    
    if (rsdp_address == 0) {
        return;
    }
    
    r1 = (acpi_rsdp_t*)(uintptr_t)rsdp_address;
    
    if (!acpi_validate_rsdp(r1)) {
        return;
    }
    
    rsdp = (acpi_rsdp2_t*)r1;
    
    /* Check if ACPI 2.0+ */
    if (r1->revision >= 2) {
        r2 = (acpi_rsdp2_t*)r1;
        if (acpi_validate_rsdp2(r2)) {
            if (r2->xsdt_address != 0) {
                xsdt = (acpi_xsdt_t*)(uintptr_t)r2->xsdt_address;
                if (acpi_validate_sdt(&xsdt->header)) {
                    use_xsdt = 1;
                } else {
                    xsdt = NULL;
                }
            }
        }
    }
    
    /* Try RSDT if XSDT not available */
    if (!use_xsdt && r1->rsdt_address != 0) {
        rsdt = (acpi_rsdt_t*)(uintptr_t)r1->rsdt_address;
        if (!acpi_validate_sdt(&rsdt->header)) {
            rsdt = NULL;
            return;
        }
    }
    
    acpi_initialized = 1;
}

int acpi_is_initialized(void) {
    return acpi_initialized;
}

acpi_sdt_header_t* acpi_find_table(const char *signature) {
    uint32_t entry_count;
    uint32_t i;
    
    if (!acpi_initialized || !signature) {
        return NULL;
    }
    
    if (use_xsdt && xsdt) {
        entry_count = (xsdt->header.length - sizeof(acpi_sdt_header_t)) / 8;
        
        for (i = 0; i < entry_count; i++) {
            acpi_sdt_header_t *header = (acpi_sdt_header_t*)(uintptr_t)xsdt->entries[i];
            
            if (!header) {
                continue;
            }
            
            if (acpi_memcmp(header->signature, signature, 4) == 0) {
                if (acpi_validate_sdt(header)) {
                    return header;
                }
            }
        }
    } else if (rsdt) {
        entry_count = (rsdt->header.length - sizeof(acpi_sdt_header_t)) / 4;
        
        for (i = 0; i < entry_count; i++) {
            acpi_sdt_header_t *header = (acpi_sdt_header_t*)(uintptr_t)rsdt->entries[i];
            
            if (!header) {
                continue;
            }
            
            if (acpi_memcmp(header->signature, signature, 4) == 0) {
                if (acpi_validate_sdt(header)) {
                    return header;
                }
            }
        }
    }
    
    return NULL;
}

void acpi_print_info(CONSOLE *con) {
    if (!con || !acpi_initialized) {
        return;
    }
    
    console_printf(con, "ACPI Information:\n");
    
    if (rsdp) {
        console_printf(con, "  RSDP found at 0x%p\n", (void*)rsdp);
        console_printf(con, "  Revision: %u\n", (unsigned int)rsdp->rsdp1.revision);
        console_printf(con, "  OEM ID: %.6s\n", rsdp->rsdp1.oem_id);
    }
    
    if (use_xsdt && xsdt) {
        console_printf(con, "  Using XSDT at 0x%p\n", (void*)xsdt);
    } else if (rsdt) {
        console_printf(con, "  Using RSDT at 0x%p\n", (void*)rsdt);
    }
    
    /* Print available tables */
    uint32_t entry_count;
    uint32_t i;
    
    if (use_xsdt && xsdt) {
        entry_count = (xsdt->header.length - sizeof(acpi_sdt_header_t)) / 8;
        console_printf(con, "  Tables (%u):\n", entry_count);
        
        for (i = 0; i < entry_count; i++) {
            acpi_sdt_header_t *header = (acpi_sdt_header_t*)(uintptr_t)xsdt->entries[i];
            if (header) {
                console_printf(con, "    %.4s at 0x%p\n", header->signature, (void*)header);
            }
        }
    } else if (rsdt) {
        entry_count = (rsdt->header.length - sizeof(acpi_sdt_header_t)) / 4;
        console_printf(con, "  Tables (%u):\n", entry_count);
        
        for (i = 0; i < entry_count; i++) {
            acpi_sdt_header_t *header = (acpi_sdt_header_t*)(uintptr_t)rsdt->entries[i];
            if (header) {
                console_printf(con, "    %.4s at 0x%p\n", header->signature, (void*)header);
            }
        }
    }
}

void acpi_print_table(CONSOLE *con, acpi_sdt_header_t *header) {
    if (!con || !header) {
        return;
    }
    
    console_printf(con, "ACPI Table %.4s:\n", header->signature);
    console_printf(con, "  Length: %u bytes\n", header->length);
    console_printf(con, "  Revision: %u\n", (unsigned int)header->revision);
    console_printf(con, "  OEM ID: %.6s\n", header->oem_id);
    console_printf(con, "  OEM Table ID: %.8s\n", header->oem_table_id);
}

uint32_t acpi_get_cpu_count(void) {
    acpi_madt_t *madt;
    uint8_t *ptr;
    uint8_t *end;
    uint32_t count = 0;
    
    if (!acpi_initialized) {
        return 1; /* Default to 1 CPU */
    }
    
    madt = (acpi_madt_t*)acpi_find_table(ACPI_SIG_MADT);
    if (!madt) {
        return 1;
    }
    
    ptr = (uint8_t*)madt + sizeof(acpi_madt_t);
    end = (uint8_t*)madt + madt->header.length;
    
    while (ptr < end) {
        acpi_madt_entry_header_t *entry = (acpi_madt_entry_header_t*)ptr;
        
        if (entry->type == ACPI_MADT_TYPE_LOCAL_APIC) {
            acpi_madt_local_apic_t *lapic = (acpi_madt_local_apic_t*)entry;
            if (lapic->flags & 1) { /* CPU enabled */
                count++;
            }
        }
        
        ptr += entry->length;
    }
    
    return count > 0 ? count : 1;
}

uint64_t acpi_get_local_apic_address(void) {
    acpi_madt_t *madt;
    
    if (!acpi_initialized) {
        return 0xFEE00000; /* Default local APIC address */
    }
    
    madt = (acpi_madt_t*)acpi_find_table(ACPI_SIG_MADT);
    if (!madt) {
        return 0xFEE00000;
    }
    
    return (uint64_t)madt->local_apic_address;
}

/*
 * ==================== S5 shutdown and reset ====================
 */

/* FADT register fields we care about (ACPI 2.0+, byte offsets). */
#define FADT_OFF_PM1A_CNT      64
#define FADT_OFF_PM1B_CNT      68
#define FADT_OFF_PM1_CNT_LEN   87
#define FADT_OFF_REV2          0

static uint32_t fadt_u32(acpi_sdt_header_t *fadt, uint32_t off) {
    const uint8_t *p = (const uint8_t*)fadt;
    uint32_t v;

    if (fadt->length < off + 4) {
        return 0;
    }

    v = (uint32_t)p[off] |
        ((uint32_t)p[off + 1] << 8) |
        ((uint32_t)p[off + 2] << 16) |
        ((uint32_t)p[off + 3] << 24);
    return v;
}

/*
 * Parse the DSDT for "\_S5" and return SLP_TYPa (0..7).
 *
 * The object is declared as:
 *     0x08 "_S5_" 0x12 <PkgLen> <NumElements> <Int0> ...
 * This is the byte sequence used by every sane DSDT; instead of a full
 * AML interpreter we scan for the literal bytes and decode the first
 * integer element of the package. Values appear either as Zero/OneOp
 * or BytePrefix.
 */
static int dsdt_find_s5_slp_typ(uint8_t *out_value) {
    acpi_sdt_header_t *dsdt;
    const uint8_t *p;
    const uint8_t *end;
    uint32_t len;
    uint32_t i;
    static const uint8_t s5_name_seq[5] = { 0x08, 0x5F, 0x53, 0x35, 0x5F }; /* NameOp "_S5_" */

    /*
     * DSDT is not an entry of RSDT/XSDT: it lives in memory and is
     * referenced FROM the FADT (DSDT address at byte offset 40).
     */
    {
        acpi_sdt_header_t *fadt = acpi_find_table(ACPI_SIG_FADT);
        if (!fadt || fadt->length < 44) {
            return 0;
        }
        {
            uint32_t dsdt_phys = fadt_u32(fadt, 40);
            if (dsdt_phys == 0) {
                return 0;
            }
            dsdt = (acpi_sdt_header_t*)(uintptr_t)dsdt_phys;
        }
    }

    /* Validate the DSDT header (signature + checksum) before scanning. */
    if (acpi_memcmp(dsdt->signature, "DSDT", 4) != 0 ||
        acpi_checksum(dsdt, dsdt->length) != 0 ||
        dsdt->length < 64) {
        return 0;
    }

    p    = (const uint8_t*)dsdt;
    end  = p + dsdt->length;
    len  = dsdt->length;

    for (i = 0; i + 12 < len; i++) {
        int k;
        int match = 1;

        for (k = 0; k < 5; k++) {
            if (p[i + k] != s5_name_seq[k]) {
                match = 0;
                break;
            }
        }
        if (!match) {
            continue;
        }

        /* PackageOp expected right after the name */
        if (p[i + 5] != 0x12) {
            continue;
        }

        /* Decode PkgLength (one-byte form covers our small package). */
        {
            uint32_t j = i + 6;
            uint8_t len_byte = p[j];
            uint32_t pkg_len;

            if ((len_byte & 0xC0) == 0xC0) {
                /* 3-byte form: len_byte + 2 bytes (ignore high bits) */
                pkg_len = (uint32_t)((len_byte & 0x0F) |
                          ((uint32_t)p[j + 1] << 4) |
                          ((uint32_t)p[j + 2] << 12));
                if (pkg_len < 4 || j + 3 + pkg_len > (uint32_t)len) {
                    continue;
                }
                j += 3;
            } else if ((len_byte & 0xC0) == 0x40) {
                /* 2-byte form */
                pkg_len = (uint32_t)((len_byte & 0x0F) |
                          ((uint32_t)p[j + 1] << 4));
                if (pkg_len < 4 || j + 2 + pkg_len > (uint32_t)len) {
                    continue;
                }
                j += 2;
            } else {
                pkg_len = len_byte;
                if (pkg_len < 3 || j + 1 + pkg_len > (uint32_t)len) {
                    continue;
                }
                j += 1;
            }

            if (j < end - 2) {
                /* NumElements byte, then first package element */
                uint8_t n_el = p[j];
                uint8_t elem = p[j + 1];
                (void)n_el;

                if (elem == 0x00) {              /* ZeroOp  */
                    *out_value = 0;
                    return 1;
                } else if (elem == 0x01) {       /* OneOp   */
                    *out_value = 1;
                    return 1;
                } else if (elem == 0x0A) {       /* BytePrefix */
                    if (j + 2 < (uint32_t)(end - p)) {
                        *out_value = p[j + 2];
                        return 1;
                    }
                } else if (elem == 0x0B) {       /* WordPrefix */
                    if (j + 3 < (uint32_t)(end - p)) {
                        *out_value = p[j + 2] & 0x07;
                        return 1;
                    }
                } else if (elem == 0x0C) {       /* DWordPrefix */
                    if (j + 5 < (uint32_t)(end - p)) {
                        *out_value = p[j + 2] & 0x07;
                        return 1;
                    }
                }
            }
        }
    }

    return 0;
}

int acpi_poweroff(void) {
    acpi_sdt_header_t *fadt;
    uint32_t pm1a_cnt;
    uint8_t slp_typ;
    uint16_t value;

    if (!acpi_initialized) {
        return -1;
    }

    fadt = acpi_find_table(ACPI_SIG_FADT);
    if (!fadt) {
        return -1;
    }

    pm1a_cnt = fadt_u32(fadt, FADT_OFF_PM1A_CNT);
    if (pm1a_cnt == 0 || pm1a_cnt >= 0x10000U) {
        return -1;
    }

    /* The PM1a_CNT register must be at least 2 bytes wide. */
    {
        uint8_t pm1_cnt_len = 0;
        if (fadt->length > FADT_OFF_PM1_CNT_LEN) {
            pm1_cnt_len = ((const uint8_t*)fadt)[FADT_OFF_PM1_CNT_LEN];
        }
        if (pm1_cnt_len > 0 && pm1_cnt_len < 2) {
            return -1;
        }
    }

    if (!dsdt_find_s5_slp_typ(&slp_typ)) {
        return -1;
    }

    if (slp_typ > 7) {
        slp_typ &= 0x07;
    }

    value = (uint16_t)((slp_typ << 10) | (1U << 13)); /* SLP_TYP | SLP_EN */
    outw((uint16_t)pm1a_cnt, value);

    /* Should never return; belt-and-braces idle loop. */
    for (;;) {
        __asm__ volatile("hlt");
    }
}

int acpi_reset(void) {
    acpi_sdt_header_t *fadt;

    if (!acpi_initialized) {
        return -1;
    }

    fadt = acpi_find_table(ACPI_SIG_FADT);
    if (!fadt || fadt->revision < 2) {
        return -1;
    }

    /*
     * FADT for ACPI >= 2: RESET_REG is a GAS (12 bytes) at offset 116:
     *   [116] address_space_id (must be 1 = SystemMemory or 0 = SystemIO)
     *   [117] bit_width, [118] bit_offset, [119] access_size
     *   [120..127] 64-bit address
     * RESET_TYPE (byte) at offset 128.
     */
    if (fadt->length < 129) {
        return -1;
    }
    {
        const uint8_t *p = (const uint8_t*)fadt;
        uint8_t gas_space = p[116];
        uint64_t gas_addr = (uint64_t)p[120] | ((uint64_t)p[121] << 8) |
                            ((uint64_t)p[122] << 16) | ((uint64_t)p[123] << 24) |
                            ((uint64_t)p[124] << 32) | ((uint64_t)p[125] << 40) |
                            ((uint64_t)p[126] << 48) | ((uint64_t)p[127] << 56);
        uint8_t reset_type = p[128];

        if (gas_addr == 0 || reset_type == 0) {
            return -1;
        }

        /* Only SystemIO (0) or SystemMemory (1) bridge practically. We
         * implement the SystemIO case which is what real and virtual
         * firmware both use. */
        if (gas_space == 0 && gas_addr < 0x10000ULL) {
            outb((uint16_t)gas_addr, reset_type);
            return 0;
        }
    }

    return -1;
}
