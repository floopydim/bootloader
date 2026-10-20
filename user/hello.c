/*
 * hello.elf — first CFOS ring 3 program.
 *
 * Checks the whole ring-3 path (per-task address space, IRET enter,
 * INT 0x80 syscalls, DPL-3 gate, faults→kill path): prints a banner,
 * waits for keys and echoes them uppercase, exits on 'q'.
 */

#include "common.h"

void main(void) {
    u64 pid;
    u64 seconds;

    write_str("hello from ring 3!\n");

    pid = sys0(SYS_GETPID);
    seconds = sys0(SYS_TIME);

    {
        char buf[6];
        u64 digits = 0;
        u64 tmp = pid;
        /* tiny decimal print, reversed */
        do {
            buf[digits++] = (char)('0' + tmp % 10);
            tmp /= 10;
        } while (tmp && digits < 5);
        /* reverse in place */
        for (u64 i = 0; i < digits / 2; i++) {
            char t = buf[i];
            buf[i] = buf[digits - 1 - i];
            buf[digits - 1 - i] = t;
        }
        buf[digits] = '\0';
        write_str("pid: ");
        write_str(buf);
        write_str("\n");
    }

    if (seconds > 99) {
        /* Honor uptime with three digits on a slow boot */
        write_str("kernel uptime: >99s\n");
    } else {
        char buf[3];
        buf[0] = (char)('0' + seconds / 10);
        buf[1] = (char)('0' + seconds % 10);
        buf[2] = '\0';
        write_str("kernel uptime: ");
        write_str(buf);
        write_str(" s\n");
    }

    write_str("echo mode: type keys, 'q' quits\n");
    write_str("> ");

    for (;;) {
        char ch;
        sys3(SYS_READ, 0, (u64)&ch, 1);

        if (ch == 'q' || ch == 'Q') {
            write_str("\nbye!\n");
            sys3(SYS_EXIT, 42, 0, 0);
        }

        /* echo an uppercase letter (demo of data flow back to ring 3) */
        if (ch >= 'a' && ch <= 'z') {
            ch = (char)(ch - 'a' + 'A');
        }

        sys3(SYS_WRITE, 1, (u64)&ch, 1);
    }
}
