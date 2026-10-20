#include "interrupts.h"
#include "idt.h"
#include "panic.h"
#include "pic.h"
#include "pit.h"
#include "exceptions.h"
#include "userproc.h"
#include <stddef.h>
#include <stdint.h>

extern void *isr_stub_table[256];

static irq_handler_t irq_handlers[16];

void interrupts_set_console(CONSOLE *con) {
    exceptions_set_console(con);
}

void interrupts_enable(void) {
    __asm__ __volatile__("sti");
}

void interrupts_disable(void) {
    __asm__ __volatile__("cli");
}

void irq_register_handler(uint8_t irq, irq_handler_t handler) {
    if (irq < 16) {
        irq_handlers[irq] = handler;
    }
}

void irq_unregister_handler(uint8_t irq) {
    if (irq < 16) {
        irq_handlers[irq] = 0;
    }
}

void irq_mask(uint8_t irq) {
    pic_irq_mask(irq);
}

void irq_unmask(uint8_t irq) {
    pic_irq_unmask(irq);
}

void timer_init(uint32_t hz) {
    pit_init(hz);
}

void timer_start(uint32_t hz) {
    pit_start(hz);
}

void timer_stop(void) {
    pit_stop();
}

uint64_t timer_ticks(void) {
    return pit_ticks();
}

uint32_t timer_hz(void) {
    return pit_hz();
}

/*
 * Локальный APIC на современных прошивках может остаться включённым
 * после UEFI/SMM — тогда часть прерываний уходит в него, а legacy PIC
 * их никогда не увидит (эффект: "клавиатура молчит"). Отключаем.
 * В x2APIC-режиме не трогаем: возврат из x2APIC — отдельная процедура.
 */
static void lapic_disable_if_enabled(void) {
    uint32_t lo, hi;

    __asm__ __volatile__("rdmsr" : "=a"(lo), "=d"(hi) : "c"(0x1B));

    if (lo & (1U << 10)) {
        return; /* x2APIC mode — не лезем */
    }

    if (lo & (1U << 11)) {
        lo &= ~(1U << 11);
        __asm__ __volatile__("wrmsr" : : "a"(lo), "d"(hi), "c"(0x1B));
    }
}

void interrupts_init(void) {
    int i;

    interrupts_disable();
    lapic_disable_if_enabled();

    idt_init();

    for (i = 0; i < 16; i++) {
        irq_handlers[i] = 0;
    }

    for (i = 0; i < 256; i++) {
        idt_set_gate((uint8_t)i, (uint64_t)(uintptr_t)isr_stub_table[i], 0x8E);
    }

    pic_remap_all_masked();

    /*
     * Только каскад PIC оставляем доступным.
     * IRQ0 включается вручную из shell через piton.
     */
    pic_irq_unmask(2);

    /*
     * INT 0x80 — syscall-гейт для ring 3: должен быть вызываемым из
     * CPL 3, поэтому DPL=3 (0xEE: P=1, DPL=3, interrupt gate).
     */
    idt_set_gate(0x80, (uint64_t)(uintptr_t)isr_stub_table[0x80], 0xEE);

    idt_load();
}

void interrupt_dispatch(INTERRUPT_FRAME *frame) {
    uint64_t vec;

    if (!frame) {
        panic_set_stage("interrupt dispatch");
        panic("interrupt_dispatch received NULL frame");
    }

    vec = frame->vector;

    if (vec < 32) {
        handle_exception(frame);
        /* Will not return for ring 3: userproc_handle_fault() inside
         * handle_exception restores the kernel context instead. */
    }

    if (vec == 0x80) {
        userproc_handle_syscall(frame);
        return;
    }

    if (vec >= 32 && vec < 48) {
        uint8_t irq = (uint8_t)(vec - 32);
        int spurious = pic_irq_is_spurious(irq);

        if (!spurious) {
            if (irq == 0) {
                pit_handle_irq();
            }

            if (irq_handlers[irq]) {
                irq_handlers[irq]();
            }
        }

        if (irq == 15 && spurious) {
            /*
             * Спириозный IRQ15: слейв EOI слать нельзя (иначе каскад
             * "залипает" и теряются IRQ8-15), а мастеру послать нужно —
             * у него всё ещё в ин-сервисе каскадная линия IRQ2.
             */
            pic_send_eoi_master_only();
            return;
        }

        pic_send_eoi(irq);
        return;
    }
}
