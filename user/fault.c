/*
 * fault.elf — deliberate segfault in ring 3.
 *
 * Verifies the exception->kill path: page fault, task torn down,
 * kernel keeps running, only this task dies.
 */

#include "common.h"

void main(void) {
    volatile u64 *p;
    u64 pid;

    pid = sys0(SYS_GETPID);
    (void)pid;

    write_str("fault: about to deref NULL\n");

    p = (volatile u64 *)0;
    *p = 0xDEADBEEFULL;

    write_str("fault: UNREACHABLE - should never print this\n");
}
