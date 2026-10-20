#include "pic.h"
#include "io.h"

#define PIC1_COMMAND 0x20
#define PIC1_DATA    0x21
#define PIC2_COMMAND 0xA0
#define PIC2_DATA    0xA1
#define PIC_EOI      0x20

void pic_remap_all_masked(void) {
    outb(PIC1_COMMAND, 0x11);
    io_wait();
    outb(PIC2_COMMAND, 0x11);
    io_wait();

    outb(PIC1_DATA, 0x20);
    io_wait();
    outb(PIC2_DATA, 0x28);
    io_wait();

    outb(PIC1_DATA, 0x04);
    io_wait();
    outb(PIC2_DATA, 0x02);
    io_wait();

    outb(PIC1_DATA, 0x01);
    io_wait();
    outb(PIC2_DATA, 0x01);
    io_wait();

    outb(PIC1_DATA, 0xFF);
    outb(PIC2_DATA, 0xFF);
}

void pic_send_eoi(uint8_t irq) {
    if (irq >= 8) {
        outb(PIC2_COMMAND, PIC_EOI);
    }
    outb(PIC1_COMMAND, PIC_EOI);
}

void pic_send_eoi_master_only(void) {
    outb(PIC1_COMMAND, PIC_EOI);
}

/*
 * Проверка спириозного прерывания (read ISR через OCW3).
 * Спириозные IRQ7/IRQ15 на реальном 8259 — классика: без проверки
 * EOI в слейв может "заложить" каскад и потерять IRQ8-15.
 */
int pic_irq_is_spurious(uint8_t irq) {
    uint8_t isr;

    if (irq < 8) {
        outb(PIC1_COMMAND, 0x0B); /* OCW3: read ISR */
        isr = inb(PIC1_COMMAND);
        return (isr & (1u << irq)) == 0;
    }

    if (irq == 15) {
        outb(PIC2_COMMAND, 0x0B); /* OCW3: read ISR */
        isr = inb(PIC2_COMMAND);
        return (isr & (1u << 7)) == 0;
    }

    return 0;
}

void pic_irq_mask(uint8_t irq) {
    uint16_t port;
    uint8_t value;

    if (irq < 8) {
        port = PIC1_DATA;
    } else {
        port = PIC2_DATA;
        irq -= 8;
    }

    value = inb(port);
    value |= (uint8_t)(1u << irq);
    outb(port, value);
}

void pic_irq_unmask(uint8_t irq) {
    uint16_t port;
    uint8_t value;

    if (irq < 8) {
        port = PIC1_DATA;
    } else {
        port = PIC2_DATA;
        irq -= 8;
    }

    value = inb(port);
    value &= (uint8_t)~(1u << irq);
    outb(port, value);
}
