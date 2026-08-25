/* ============================================================
 * NovaOS - Ring 3 user mode
 *   - identity paging (kernel pages supervisor-only, NXP pages user)
 *   - new GDT with user segments + TSS (Ring3 -> Ring0 stack switch)
 *   - minimal IDT: exceptions 0..31 + int 0x80 syscall gate (DPL 3)
 *   - trampoline page @ 4MB: per-API syscall stubs so existing .nxp
 *     binaries keep working unmodified (same nxp_api_t layout)
 * ============================================================ */
#include "paging.h"
#include "gfx.h"

/* console helpers from kernel.c (non-static there) */
extern void kput(char c);
extern void kputs(const char *s);
extern void kput_hex(unsigned v);

/* asm helpers from entry.asm */
extern void isr_syscall(void);
extern uint32_t isr_fault_table[];   /* 32 stub addresses */
void ring3_leave(void);

/* ---- fixed placements (linker gives only 16-byte alignment) ---- */
#define PDE_ADDR          0x00141000u
#define PT_LOW_ADDR       0x00142000u   /* maps 0 .. 4MB    */
#define PT_USER_ADDR      0x00143000u   /* maps 4 .. 8MB    */
#define SYSCALL_STK_TOP   0x001F0000u   /* Ring3->Ring0 stack via TSS */

static uint32_t *const pde = (uint32_t *)PDE_ADDR;
static uint32_t *const ptl = (uint32_t *)PT_LOW_ADDR;
static uint32_t *const ptu = (uint32_t *)PT_USER_ADDR;

static uint8_t tss[104];
static uint64_t gdt[6];
static uint64_t idt[256];

/* ---- kernel-side API table (same layout as nxp_api_t in kernel.c) ---- */
typedef uint32_t (*fn5)(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t);
typedef struct {
    uint32_t magic, version;
    fn5 putc, puts, getchar, exit;
    uint32_t scr_w, scr_h;
    fn5 pixel, fill_rect, text, getkey, mouse, get_pixel;
} kapi_t;
static const kapi_t *g_api;

/* register frame pushed by pushad + CPU fault frame after it:
   [0]edi [1]esi [2]ebp [3]esp [4]ebx [5]edx [6]ecx [7]eax
   [8]err [9]eip [10]cs [11]eflags                              */
typedef struct { uint32_t r[12]; } regs_t;

/* syscall numbers == trampoline stub order */
enum { SYS_PUTC, SYS_PUTS, SYS_GETCHAR, SYS_EXIT,
       SYS_PIXEL, SYS_FILL, SYS_TEXT, SYS_GETKEY,
       SYS_MOUSE, SYS_GETPIXEL, SYS_COUNT };

uint32_t g_nxp_exit_stub;

/* ---- accept only user-space pointers from user programs ---- */
static uint32_t uptr(uint32_t p)
{
    return (p >= 0x00300000u && p < 0x00600000u) ? p : 0;
}

void syscall_dispatch(regs_t *r)
{
    uint32_t ret = 0;
    if (r->r[7] >= SYS_COUNT) { r->r[7] = 0; return; }
    uint32_t *a = (uint32_t *)uptr(r->r[4]);   /* EBX = args block */
    if (!a) { r->r[7] = 0; return; }

    switch (r->r[7]) {
    case SYS_PUTC:     g_api->putc(a[0], 0, 0, 0, 0); break;
    case SYS_PUTS:     g_api->puts(uptr(a[0]), 0, 0, 0, 0); break;
    case SYS_GETCHAR:  ret = g_api->getchar(0, 0, 0, 0, 0); break;
    case SYS_EXIT:     ring3_leave();           /* never returns */
    case SYS_PIXEL:    g_api->pixel(a[0], a[1], a[2], 0, 0); break;
    case SYS_FILL:     g_api->fill_rect(a[0], a[1], a[2], a[3], a[4]); break;
    case SYS_TEXT:     g_api->text(a[0], a[1], uptr(a[2]), a[3], 0); break;
    case SYS_GETKEY:   ret = g_api->getkey(0, 0, 0, 0, 0); break;
    case SYS_MOUSE:
        ret = g_api->mouse(uptr(a[0]), uptr(a[1]), uptr(a[2]), 0, 0);
        break;
    case SYS_GETPIXEL: ret = g_api->get_pixel(a[0], a[1], 0, 0, 0); break;
    }
    r->r[7] = ret;                              /* return value -> EAX */
}

/* ---- BSOD (red screen of death) ---- */
#define BSOD_BG   0x00FF0000u          /* pure bright red */
#define BSOD_FG   0x00FFFFFFu          /* white */

static const char * const exc_name[32] = {
    "#DE Divide Error",        "#DB Debug",            "NMI",                    "#BP Breakpoint",
    "#OF Overflow",            "#BR Bound Range",      "#UD Invalid Opcode",     "#NM Device N/A",
    "#DF Double Fault",        "CoSeg Overrun",        "#TS Invalid TSS",        "#NP Seg Not Present",
    "#SS Stack Fault",         "#GP General Protection","#PF Page Fault",        "Reserved",
    "#MF x87 FPU Error",      "#AC Alignment Check",  "#MC Machine Check",      "#XM SIMD Exception",
    "#VE Virtualization",     "#CP Control Protection","Reserved",              "Reserved",
    "Reserved",               "Reserved",              "Reserved",               "Reserved",
    "Reserved",               "Reserved",              "Reserved",               "Reserved",
};

/* format "0xXXXXXXXX" into buf (11 bytes incl nul) */
static int fmt_hex(char *buf, uint32_t v)
{
    static const char h[] = "0123456789ABCDEF";
    buf[0]='0'; buf[1]='x';
    for (int i = 0; i < 8; i++) buf[2+i] = h[(v >> (28 - i*4)) & 0xF];
    buf[10] = 0;
    return 10;
}

/* append string, return new length */
static int app_str(char *buf, int n, const char *s)
{
    while (*s) buf[n++] = *s++;
    buf[n] = 0;
    return n;
}

static void bsod_show(regs_t *r, uint32_t vector, uint32_t cr2)
{
    char line[80];
    int  n;

    /* serial dump first (kputs also hits VGA but we overwrite it next) */
    kputs("\n!!! FATAL: "); kputs(exc_name[vector & 31]); kputs(" !!!\n");
    kputs("EIP="); kput_hex(r->r[9]); kputs(" ERR="); kput_hex(r->r[8]);
    kputs(" CR2="); kput_hex(cr2); kputs("\n");
    kputs("EAX="); kput_hex(r->r[7]); kputs(" EBX="); kput_hex(r->r[4]);
    kputs(" ECX="); kput_hex(r->r[6]); kputs(" EDX="); kput_hex(r->r[5]); kputs("\n");
    kputs("ESI="); kput_hex(r->r[1]); kputs(" EDI="); kput_hex(r->r[0]);
    kputs(" EBP="); kput_hex(r->r[2]); kputs(" ESP="); kput_hex(r->r[3]); kputs("\n");
    kputs("CS=");  kput_hex(r->r[10]); kputs(" EFL="); kput_hex(r->r[11]); kputs("\n");

    if (gfx_active()) {
        /* fill screen with pure bright red + set text bg */
        gfx_set_bg_rgb(BSOD_BG);
        gfx_clear();

        int y = 80;
        gfx_text(460, y, ":(", BSOD_FG);                   y += 60;
        gfx_text(200, y, "NovaOS ran into a problem and needs to halt.", BSOD_FG);  y += 28;
        gfx_text(240, y, "We're collecting fault info, then stopping for you.", BSOD_FG); y += 50;

        /* exception name */
        gfx_text(200, y, exc_name[vector & 31], BSOD_FG);  y += 40;

        /* EIP / ERR / CR2 */
        n = app_str(line, 0, "EIP: ");   n += fmt_hex(line+n, r->r[9]);
        n = app_str(line, n, "   ERR: "); n += fmt_hex(line+n, r->r[8]);
        n = app_str(line, n, "   CR2: "); n += fmt_hex(line+n, cr2);
        gfx_text(200, y, line, BSOD_FG);  y += 25;

        /* CS / EFLAGS */
        n = app_str(line, 0, "CS:  ");    n += fmt_hex(line+n, r->r[10]);
        n = app_str(line, n, "   EFL: "); n += fmt_hex(line+n, r->r[11]);
        gfx_text(200, y, line, BSOD_FG);  y += 40;

        /* registers */
        gfx_text(200, y, "Registers:", BSOD_FG);  y += 25;
        n = app_str(line, 0, "EAX: "); n += fmt_hex(line+n, r->r[7]);
        n = app_str(line, n, "  EBX: "); n += fmt_hex(line+n, r->r[4]);
        n = app_str(line, n, "  ECX: "); n += fmt_hex(line+n, r->r[6]);
        n = app_str(line, n, "  EDX: "); n += fmt_hex(line+n, r->r[5]);
        gfx_text(200, y, line, BSOD_FG);  y += 25;
        n = app_str(line, 0, "ESI: "); n += fmt_hex(line+n, r->r[1]);
        n = app_str(line, n, "  EDI: "); n += fmt_hex(line+n, r->r[0]);
        n = app_str(line, n, "  EBP: "); n += fmt_hex(line+n, r->r[2]);
        n = app_str(line, n, "  ESP: "); n += fmt_hex(line+n, r->r[3]);
        gfx_text(200, y, line, BSOD_FG);  y += 50;

        gfx_text(300, y, "Halted. Power off to restart.", BSOD_FG);
    }
}

void fault_dispatch(regs_t *r, uint32_t vector)   /* exception 0..31, never returns */
{
    uint32_t cr2;
    __asm__ volatile ("mov %%cr2, %0" : "=r"(cr2));

    if ((r->r[10] & 3) == 3) {                  /* fault in Ring 3 */
        kputs("\n[nxp] user fault: ");
        kputs(exc_name[vector & 31]);
        kputs("  eip=");  kput_hex(r->r[9]);
        kputs(" err=");   kput_hex(r->r[8]);
        kputs(" cr2=");   kput_hex(cr2);
        kput('\n');
        ring3_leave();                          /* kill the program */
    }

    /* kernel fault — show BSOD and halt */
    bsod_show(r, vector, cr2);
    __asm__ volatile ("cli");
    for (;;) __asm__ volatile ("hlt");
}

/* ---- trampoline page: cdecl stub -> int 0x80 -> kernel ---- */
static void emit_stub(uint8_t *p, uint32_t sysno, int nargs)
{
    *p++ = 0xB8; *(uint32_t *)p = sysno; p += 4;        /* mov eax,sysno */
    for (int i = 0; i < nargs; i++) {
        *p++ = 0x8B; *p++ = 0x54; *p++ = 0x24;          /* mov edx,[esp+d] */
        *p++ = (uint8_t)(4 + i * 4);
        *p++ = 0x89; *p++ = 0x15;                       /* mov [abs],edx  */
        *(uint32_t *)p = NXP_TRAMP_ARGS + i * 4; p += 4;
    }
    *p++ = 0xBB; *(uint32_t *)p = NXP_TRAMP_ARGS; p += 4; /* mov ebx,args */
    *p++ = 0xCD; *p++ = 0x80;                           /* int 0x80      */
    *p++ = 0xC3;                                        /* ret           */
}

void ring3_setup_tramp(void)
{
    static const uint8_t nargs[SYS_COUNT] = { 1,1,0,0,3,5,4,0,3,2 };
    uint8_t *p = (uint8_t *)NXP_TRAMP_BASE;
    uint32_t stub[SYS_COUNT];

    for (int s = 0; s < SYS_COUNT; s++) {
        stub[s] = (uint32_t)p;
        emit_stub(p, (uint32_t)s, nargs[s]);
        p += 13 + nargs[s] * 9;
    }
    g_nxp_exit_stub = stub[SYS_EXIT];

    uint32_t *t = (uint32_t *)NXP_TRAMP_API;   /* mirror nxp_api_t */
    t[0]  = g_api->magic;      t[1]  = g_api->version;
    t[2]  = stub[SYS_PUTC];    t[3]  = stub[SYS_PUTS];
    t[4]  = stub[SYS_GETCHAR]; t[5]  = stub[SYS_EXIT];
    t[6]  = g_api->scr_w;      t[7]  = g_api->scr_h;
    t[8]  = stub[SYS_PIXEL];   t[9]  = stub[SYS_FILL];
    t[10] = stub[SYS_TEXT];    t[11] = stub[SYS_GETKEY];
    t[12] = stub[SYS_MOUSE];   t[13] = stub[SYS_GETPIXEL];
}

/* ---- descriptor helpers ---- */
static uint64_t mk_desc(uint32_t base, uint32_t limit, uint8_t acc, uint8_t flg)
{
    return ((uint64_t)(limit & 0xFFFF))
         | ((uint64_t)(base & 0xFFFFFF) << 16)
         | ((uint64_t)acc << 40)
         | ((uint64_t)((limit >> 16) & 0xF) << 48)
         | ((uint64_t)flg << 52)
         | ((uint64_t)((base >> 24) & 0xFF) << 56);
}

static void set_gate(int vec, void (*fn)(void), uint8_t flags)
{
    uint32_t off = (uint32_t)fn;
    uint32_t *e = (uint32_t *)&idt[vec];
    /* 32-bit IDT gate layout:
     * [0] bits 0-15 : offset 15:0
     * [0] bits 16-31: selector (0x08 = kernel code)
     * [1] bits 0-7  : reserved (0)
     * [1] bits 8-15 : flags (type/attr, e.g. 0x8E / 0xEE)
     * [1] bits 16-31: offset 31:16                             */
    e[0] = (off & 0xFFFF) | (0x08u << 16);
    e[1] = ((off >> 16) << 16) | ((uint32_t)flags << 8);
}

static void map_init(void)
{
    /* 0..4MB: supervisor everywhere except NXP code 3MB..4MB (user RW) */
    for (int i = 0; i < 1024; i++) {
        uint32_t us = (i >= 0x300 && i < 0x400) ? 0x4 : 0x0;
        ptl[i] = ((uint32_t)i << 12) | 0x3 | us;
    }
    /* 4..8MB: user RW for trampoline 0x400000 (i=0) + stack up to 0x500000 (i=0x100).
       ptu[i] maps VA = 4MB + i*4K, so i=0 -> 0x400000, i=0x100 -> 0x500000. */
    for (int i = 0; i < 1024; i++) {
        uint32_t us = (i <= 0x100) ? 0x4 : 0x0;
        ptu[i] = ((uint32_t)(0x400 + i) << 12) | 0x3 | us;
    }
    for (int i = 0; i < 1024; i++) pde[i] = 0;
    /* PDE.US MUST be 1 for Ring3 to reach anything under 8MB; individual
       PTEs still protect low kernel pages (US=0 denies user access). */
    pde[0] = PT_LOW_ADDR  | 0x7;                /* present | RW | US */
    pde[1] = PT_USER_ADDR | 0x7;                /* present | RW | US */
    for (int i = 2; i < 4; i++)                 /* 8..16MB: 4MB pages */
        pde[i] = ((uint32_t)i << 22) | 0x83;    /* present|RW|PS (supervisor only) */
    pde[0xFD000000u >> 22] = 0xFD000000u | 0x93; /* LFB: +PCD, supervisor only */
}

void ring3_init(const void *api_table)
{
    g_api = (const kapi_t *)api_table;

    /* GDT: null, kcode 08, kdata 10, ucode 18, udata 20, TSS 28 */
    gdt[0] = 0;
    gdt[1] = mk_desc(0, 0xFFFFF, 0x9A, 0xC);
    gdt[2] = mk_desc(0, 0xFFFFF, 0x92, 0xC);
    gdt[3] = mk_desc(0, 0xFFFFF, 0xFA, 0xC);
    gdt[4] = mk_desc(0, 0xFFFFF, 0xF2, 0xC);
    gdt[5] = mk_desc((uint32_t)tss, 103, 0x89, 0x0);

    uint32_t *t = (uint32_t *)tss;              /* minimal TSS */
    t[1] = SYSCALL_STK_TOP;                     /* ESP0 */
    t[2] = 0x10;                                /* SS0  */
    *(uint16_t *)&tss[102] = 104;               /* IOPB: deny all ports */

    for (int i = 0; i < 32; i++) {
        set_gate(i, (void(*)(void))isr_fault_table[i], 0x8E);
    }
    set_gate(0x80, isr_syscall, 0xEE);          /* syscall gate, DPL 3 */

    /* mask the PIC: everything is polled, no vectors 32+ are installed */
    __asm__ volatile ("outb %0, %1" : : "a"((uint8_t)0xFF), "Nd"(0xA1));
    __asm__ volatile ("outb %0, %1" : : "a"((uint8_t)0xFF), "Nd"(0x21));

    map_init();

    /* paging on */
    __asm__ volatile ("mov %0, %%cr3" : : "r"(PDE_ADDR));
    uint32_t v;
    __asm__ volatile ("mov %%cr4, %0" : "=r"(v));
    __asm__ volatile ("mov %0, %%cr4" : : "r"(v | 0x10));       /* PSE */
    __asm__ volatile ("mov %%cr0, %0" : "=r"(v));
    __asm__ volatile ("mov %0, %%cr0" : : "r"(v | 0x80000000u));/* PG  */

    struct { uint16_t limit; uint32_t base; } __attribute__((packed)) gdtr;
    gdtr.limit = 6 * 8 - 1;  gdtr.base = (uint32_t)gdt;
    struct { uint16_t limit; uint32_t base; } __attribute__((packed)) idtr;
    idtr.limit = 256 * 8 - 1; idtr.base = (uint32_t)idt;

    __asm__ volatile ("lgdt %0" : : "m"(gdtr));
    __asm__ volatile (
        "mov $0x10, %%ax\n"
        "mov %%ax, %%ds\n"
        "mov %%ax, %%es\n"
        "mov %%ax, %%fs\n"
        "mov %%ax, %%gs\n"
        "mov %%ax, %%ss\n"
        "ljmp $0x08, $1f\n"
        "1:" ::: "ax");
    uint16_t sel = 0x28;
    __asm__ volatile ("ltr %0" : : "rm"(sel));
    __asm__ volatile ("lidt %0" : : "m"(idtr));
}
