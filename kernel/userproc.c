#include "userproc.h"
#include "task.h"
#include "vmm.h"
#include "pmm.h"
#include "syscall.h"
#include "gdt.h"
#include "panic.h"
#include "keyboard.h"
#include "pit.h"
#include <stddef.h>

/*
 * ================= userspace address layout =================
 *
 * PML4 index 1 (0x8000000000 .. 0xFFFFFFFFFF) is private per user
 * task; the kernel identity mapping (PML4 index 0) is shared but
 * supervisor-only, so ring 3 code cannot read or write it.
 */
#define USER_PML4_INDEX        1ULL
#define USER_BASE              0x0000008000000000ULL   /* 512 GiB  */
#define USER_IMG_MAX_BYTES     (32ULL * 1024 * 1024)
#define USER_STACK_SIZE        (64ULL * 1024)
#define USER_STACK_TOP         0x000000C000000000ULL /* 512 GiB + 256 GiB */
#define USER_SYSCALL_VECTOR    0x80

#define FL_ALIGN_UP(x, a)   (((x) + ((a) - 1)) & ~((a) - 1ULL))

/*
 * ================= ELF64 parsing =================
 */

typedef struct __attribute__((packed)) {
    uint8_t  ident[16];
    uint16_t type;
    uint16_t machine;
    uint32_t version;
    uint64_t entry;
    uint64_t phoff;
    uint64_t shoff;
    uint32_t flags;
    uint16_t ehsize;
    uint16_t phentsize;
    uint16_t phnum;
    uint16_t shentsize;
    uint16_t shnum;
    uint16_t shstrndx;
} user_elf64_ehdr_t;

typedef struct __attribute__((packed)) {
    uint32_t type;
    uint32_t flags;
    uint64_t offset;
    uint64_t vaddr;
    uint64_t paddr;
    uint64_t filesz;
    uint64_t memsz;
    uint64_t align;
} user_elf64_phdr_t;

#define PT_LOAD_HDR       1U
#define PF_X              1U
#define PF_W              2U

struct user_state {
    CONSOLE   *con;
    task_t    *task;
    uint64_t  *pml4;
    int        active;
    uint64_t   exit_code;
};

static struct user_state g_user;
static task_context_t g_scratch_ctx;

int userproc_is_active(void) {
    return g_user.active;
}

void userproc_init(void) {
    g_user.con = 0;
    g_user.task = 0;
    g_user.pml4 = 0;
    g_user.active = 0;
    g_user.exit_code = 0;
}

/*
 * ================= ELF loader =================
 */

static void user_memzero(void *dst, uint64_t size) {
    uint8_t *p = (uint8_t*)dst;
    while (size) {
        *p++ = 0;
        size--;
    }
}

static void user_memcpy(void *dst, const void *src, uint64_t size) {
    uint8_t *d = (uint8_t*)dst;
    const uint8_t *s = (const uint8_t*)src;
    while (size) {
        *d++ = *s++;
        size--;
    }
}

static int user_load_elf(CONSOLE *con, const uint8_t *image, uint64_t size,
                         uint64_t *out_entry) {
    const user_elf64_ehdr_t *ehdr;
    const user_elf64_phdr_t *phdrs;
    uint64_t i;

    if (size < sizeof(user_elf64_ehdr_t) + sizeof(user_elf64_phdr_t)) {
        console_printf(con, "run: ELF image too small (%u bytes)\n",
                       (unsigned int)size);
        return -1;
    }

    ehdr = (const user_elf64_ehdr_t*)image;

    if (ehdr->ident[0] != 0x7F ||
        ehdr->ident[1] != 'E' ||
        ehdr->ident[2] != 'L' ||
        ehdr->ident[3] != 'F') {
        console_printf(con, "run: not an ELF image\n");
        return -1;
    }

    if (ehdr->ident[4] != 2) {      /* 2 = ELFCLASS64 */
        console_printf(con, "run: only ELF64 binaries are supported\n");
        return -1;
    }

    if (ehdr->machine != 0x3E) {
        console_printf(con, "run: only x86-64 binaries are supported\n");
        return -1;
    }

    if (ehdr->phnum == 0 || ehdr->phnum > 16) {
        console_printf(con, "run: bad program header count %u\n",
                       (unsigned int)ehdr->phnum);
        return -1;
    }

    if (ehdr->phoff == 0 ||
        ehdr->phoff + (uint64_t)ehdr->phnum * ehdr->phentsize > size ||
        ehdr->phentsize < sizeof(user_elf64_phdr_t)) {
        console_printf(con, "run: bad program header table\n");
        return -1;
    }

    phdrs = (const user_elf64_phdr_t*)(image + ehdr->phoff);

    for (i = 0; i < ehdr->phnum; i++) {
        const user_elf64_phdr_t *ph = &phdrs[i];
        uint64_t vstart;
        uint64_t vend;
        uint64_t page_count;
        uint64_t phys;
        uint8_t *mapped;

        if (ph->type != PT_LOAD_HDR) {
            continue;
        }

        if (ph->memsz == 0) {
            continue;
        }

        if (ph->vaddr < USER_BASE ||
            ph->vaddr + ph->memsz >= USER_STACK_TOP - USER_STACK_SIZE ||
            ph->vaddr + ph->memsz >= USER_BASE + USER_IMG_MAX_BYTES) {
            console_printf(con, "run: segment %u outside user range "
                                "(vaddr=%p memsz=%u)\n",
                           (unsigned int)i,
                           (void*)(uintptr_t)ph->vaddr,
                           (unsigned int)ph->memsz);
            return -2;
        }

        if (ph->filesz > ph->memsz || ph->offset + ph->filesz > size) {
            console_printf(con, "run: segment %u claims more file data "
                                "than the image holds\n",
                           (unsigned int)i);
            return -2;
        }

        vstart = ph->vaddr & ~0xFFFULL;
        vend   = FL_ALIGN_UP(ph->vaddr + ph->memsz, 0x1000ULL);
        page_count = (vend - vstart) / 0x1000ULL;

        phys = (uint64_t)(uintptr_t)pmm_alloc_pages(page_count);
        if (!phys) {
            console_printf(con, "run: out of physical memory for "
                                "segment %u (%u pages)\n",
                           (unsigned int)i, (unsigned int)page_count);
            return -3;
        }

        /*
         * First map writable (kernel will write into them now; user
         * access is allowed but irrelevant during load) then adjust the
         * protection afterwards to the real segment rights.
         */
        vmm_map_range_at(g_user.pml4, vstart, phys,
                         page_count * 0x1000ULL,
                         VMM_WRITE | VMM_USER);

        /* The kernel identity map covers all physical memory, so we can
         * write the frames directly through their physical addresses. */
        mapped = (uint8_t*)(uintptr_t)phys;
        user_memzero(mapped, page_count * 0x1000ULL);
        user_memcpy(mapped + (ph->vaddr & 0xFFFULL),
                    image + ph->offset, ph->filesz);

        if (ph->flags & PF_W) {
            vmm_set_prot_at(g_user.pml4, vstart,
                            page_count * 0x1000ULL,
                            VMM_WRITE | VMM_USER | VMM_NO_EXEC);
        } else if (ph->flags & PF_X) {
            vmm_set_prot_at(g_user.pml4, vstart,
                            page_count * 0x1000ULL,
                            VMM_USER);
        } else {
            vmm_set_prot_at(g_user.pml4, vstart,
                            page_count * 0x1000ULL,
                            VMM_USER | VMM_NO_EXEC);
        }

        console_printf(con, "run: seg %u  v=%p..%p  phys=0x%x  "
                            "%c%c  filesz=%u memsz=%u\n",
                       (unsigned int)i,
                       (void*)(uintptr_t)ph->vaddr,
                       (void*)(uintptr_t)ph->vaddr + ph->memsz - 1,
                       (unsigned int)phys,
                       (ph->flags & PF_X) ? 'x' : '-',
                       (ph->flags & PF_W) ? 'w' : '-',
                       (unsigned int)ph->filesz,
                       (unsigned int)ph->memsz);
    }

    *out_entry = ehdr->entry;
    return 0;
}

static int user_map_stack(void) {
    uint64_t stack_bottom;
    uint64_t pages;
    uint64_t phys;

    stack_bottom = USER_STACK_TOP - USER_STACK_SIZE;
    pages = USER_STACK_SIZE / 0x1000ULL;

    phys = (uint64_t)(uintptr_t)pmm_alloc_pages(pages);
    if (!phys) {
        return -1;
    }

    vmm_map_range_at(g_user.pml4, stack_bottom, phys,
                     USER_STACK_SIZE,
                     VMM_WRITE | VMM_USER | VMM_NO_EXEC);

    return 0;
}

/*
 * ================= run =================
 */

extern void user_enter_asm(task_context_t *save,
                           void *frame, uint64_t cr3_new);

typedef struct {
    uint64_t rip;
    uint64_t cs;
    uint64_t rflags;
    uint64_t rsp;
    uint64_t ss;
} user_frame_t;

int userproc_run(CONSOLE *con,
                 const char *name,
                 const uint8_t *elf_image,
                 uint64_t image_size,
                 uint64_t *out_exit_code) {
    task_t *task;
    uint64_t entry;
    user_frame_t frame;
    int load_result;

    if (g_user.active) {
        console_printf(con, "run: another user program is active\n");
        return -100;
    }

    if (!con || !elf_image || image_size == 0) {
        return -101;
    }

    g_user.con = con;
    g_user.active = 1;
    g_user.exit_code = 0;

    g_user.pml4 = vmm_new_address_space();
    if (!g_user.pml4) {
        console_printf(con, "run: cannot allocate address space\n");
        g_user.active = 0;
        return -102;
    }

    /* Switch into the user space so that segment mappings happen
     * against the tables the user will run on. Kernel memory is
     * identity-mapped in that view as well. */
    vmm_activate(g_user.pml4);

    load_result = user_load_elf(con, elf_image, image_size, &entry);
    if (load_result < 0) {
        vmm_activate(vmm_kernel_pml4());
        g_user.active = 0;
        return load_result;
    }

    if (entry < USER_BASE ||
        entry >= USER_BASE + USER_IMG_MAX_BYTES) {
        console_printf(con, "run: entry point %p outside user range\n",
                       (void*)(uintptr_t)entry);
        vmm_activate(vmm_kernel_pml4());
        g_user.active = 0;
        return -4;
    }

    if (user_map_stack() < 0) {
        console_printf(con, "run: cannot allocate user stack\n");
        vmm_activate(vmm_kernel_pml4());
        g_user.active = 0;
        return -5;
    }

    vmm_activate(vmm_kernel_pml4());

    task = task_create(name, entry, 0, 1);
    if (!task) {
        console_printf(con, "run: cannot allocate task\n");
        g_user.active = 0;
        return -103;
    }

    g_user.task = task;

    /* task_create prepared a kernel (ring 0) context shape; we replace
     * the interesting fields for a ring 3 start. The `context.cr3`
     * slot doubles as the kernel-context-save slot as soon as
     * user_enter_asm runs. */
    task->context.rip = entry;
    task->context.rflags = 0x202;
    task->context.rsp = USER_STACK_TOP - 16;
    task->context.cr3 = 0;              /* overwritten by save */
    task->kernel_stack = task->stack_base + task->stack_size;

    /* Kernel stack for CPU switches on ring 3 -> ring 0 */
    tss_set_kernel_stack(task->kernel_stack);

    task_set_state(task, TASK_STATE_RUNNING);

    console_printf(con, "run: '%s' tid=%u entry=%p stack=%p "
                        "entering ring 3\n",
                   name ? name : "?",
                   (unsigned int)task->tid,
                   (void*)(uintptr_t)entry,
                   (void*)(uintptr_t)(USER_STACK_TOP - 16));

    frame.rip = entry;
    frame.cs = GDT_USER_CS;
    frame.rflags = 0x202;
    frame.rsp = USER_STACK_TOP - 16;
    frame.ss = GDT_USER_DS;

    user_enter_asm(&task->context, &frame, (uint64_t)(uintptr_t)g_user.pml4);

    /* Returned from the syscall/fault restore path. */
    g_user.active = 0;

    {
        uint64_t code = g_user.exit_code;

        task_set_state(task, TASK_STATE_TERMINATED);
        task_destroy(task);
        g_user.task = 0;

        tss_set_kernel_stack(0);

        if (out_exit_code) {
            *out_exit_code = code;
        }

        console_printf(con, "run: '%s' exited with code %u\n",
                       name ? name : "?",
                       (unsigned int)code);
        return 0;
    }
}

/*
 * ================= syscall path (INT 0x80) =================
 */

static uint64_t user_validate_read(uint64_t uaddr, uint64_t count) {
    uint64_t page;
    uint64_t end = uaddr + count;

    if (count > (64ULL * 1024)) {
        return 0;
    }

    for (page = uaddr & ~0xFFFULL; page < end; page += 0x1000ULL) {
        if (vmm_translate_current(page) == 0) {
            return 0;
        }
    }

    return 1;
}

void userproc_handle_syscall(INTERRUPT_FRAME *frame) {
    uint64_t num;
    uint64_t exit_code;
    task_t *task;

    if (!g_user.active || !g_user.task) {
        /* INT 0x80 from ring 0 or before a task was entered: ignore. */
        return;
    }

    task = g_user.task;
    num = frame->rax;

    switch (num) {
        case SYS_EXIT: {
            exit_code = frame->rdi;
            g_user.exit_code = exit_code;
            frame->rax = exit_code;

            console_printf(g_user.con,
                           "run: tid %u called exit(%u)\n",
                           (unsigned int)task->tid,
                           (unsigned int)exit_code);

            /* Restore the kernel context saved by user_enter_asm.
             * Interrupt frame remnants stay on the task kernel stack;
             * the task will be destroyed anyway. Coming back we are in
             * userproc_run() again. */
            task_switch_asm(&g_scratch_ctx, &task->context);
            break;
        }

        case SYS_WRITE: {
            uint64_t fd = frame->rdi;
            const char *ubuf = (const char*)(uintptr_t)frame->rsi;
            uint64_t count = frame->rdx;
            uint64_t i;

            if (fd != 1 && fd != 2) {
                frame->rax = (uint64_t)-1;
                break;
            }

            if (!user_validate_read((uint64_t)(uintptr_t)ubuf, count)) {
                frame->rax = (uint64_t)-9;   /* EFAULT */
                break;
            }

            /* Bound the write and drop non-printable bytes */
            if (count > 8192) {
                count = 8192;
            }

            for (i = 0; i < count; i++) {
                char c = ubuf[i];
                if (c == '\n' || c == '\t' ||
                    (c >= 0x20 && c < 0x7F)) {
                    console_putchar(g_user.con, c);
                } else {
                    console_putchar(g_user.con, '?');
                }
            }

            frame->rax = count;
            break;
        }

        case SYS_READ: {
            uint64_t fd = frame->rdi;
            char *ubuf = (char*)(uintptr_t)frame->rsi;
            uint64_t count = frame->rdx;
            char ch;

            if (fd != 0 || count == 0) {
                frame->rax = (uint64_t)-1;
                break;
            }

            if (!user_validate_read((uint64_t)(uintptr_t)ubuf, 1)) {
                frame->rax = (uint64_t)-9;   /* EFAULT */
                break;
            }

            /* Block (with interrupts on) until a key comes in. */
            for (;;) {
                ch = keyboard_buffer_getchar();
                if (ch != 0) {
                    break;
                }
                /* The syscall gate is an interrupt gate, so IF is clear
                 * here; re-enable before parking or HLT sleeps forever
                 * and pending IRQs never wake us. */
                __asm__ __volatile__("sti\n\thlt\n\tcli");
            }

            /* Copy the byte into user memory (validated above). */
            {
                char *dst = (char*)(uintptr_t)frame->rsi;
                *dst = ch;
            }

            frame->rax = 1;
            break;
        }

        case SYS_GETPID: {
            frame->rax = (uint64_t)task->tid;
            break;
        }

        case SYS_GETTIME: {
            uint64_t hz = timer_hz();
            if (hz == 0) {
                frame->rax = 0;
            } else {
                frame->rax = timer_ticks() / hz;
            }
            break;
        }

        case SYS_YIELD: {
            __asm__ volatile ("sti\n\thlt\n\tcli");
            frame->rax = 0;
            break;
        }

        default:
            frame->rax = (uint64_t)-38;  /* ENOSYS */
            break;
    }

    (void)task;
}

void userproc_handle_fault(INTERRUPT_FRAME *frame) {
    task_t *task;
    uint64_t vec;

    if (!g_user.active || !g_user.task) {
        /* Ring 0 fault: not our business (handled elsewhere). */
        return;
    }

    task = g_user.task;
    vec = frame->vector;

    g_user.exit_code = 128 + vec;   /* exit code 128+signal convention */

    console_printf(g_user.con,
                   "\nrun: tid %u faulted (vec %u, rip %p) - killed\n",
                   (unsigned int)task->tid,
                   (unsigned int)vec,
                   (void*)(uintptr_t)frame->rip);

    /* Restore the kernel context; the faulting frame stays on the task
     * kernel stack and dies with the task. */
    task_switch_asm(&g_scratch_ctx, &task->context);
}
