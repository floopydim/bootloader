#include "serial.h"
#include "io.h"

#define COM1_BASE 0x3F8

#define UART_DATA    0
#define UART_IER     1
#define UART_FIFO    2
#define UART_LCR     3
#define UART_MCR     4
#define UART_LSR     5

static int g_serial_ready = 0;

void serial_init(void) {
    /* Program COM1: 115200 baud, 8N1, FIFO enabled. */
    outb(COM1_BASE + UART_IER, 0x00);   /* no UART interrupts */
    outb(COM1_BASE + UART_LCR, 0x80);   /* DLAB on */
    outb(COM1_BASE + UART_DATA, 0x01);  /* divisor low: 115200 baud */
    outb(COM1_BASE + UART_IER, 0x00);   /* divisor high */
    outb(COM1_BASE + UART_LCR, 0x03);   /* 8 bits, no parity, 1 stop */
    outb(COM1_BASE + UART_FIFO, 0xC7);  /* FIFO on, clear, 14-byte threshold */
    outb(COM1_BASE + UART_MCR, 0x0B);   /* DTR | RTS | OUT2 */

    g_serial_ready = 1;
}

void serial_write_char(char c) {
    uint32_t spin = 0;

    if (!g_serial_ready) {
        return;
    }

    /* Wait (bounded) for the transmit holding register to empty. */
    while (spin < 0x10000U) {
        if (inb(COM1_BASE + UART_LSR) & 0x20) {
            break;
        }
        spin++;
    }

    outb(COM1_BASE + UART_DATA, (uint8_t)c);
}

void serial_mirror_char(char c) {
    if (c == '\n') {
        serial_write_char('\r');
    }
    serial_write_char(c);
}
