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
#include "novafs.h"

/* console helpers from kernel.c (non-static there) */
extern void kput(char c);
extern void kputs(const char *s);
extern void kput_hex(unsigned v);
extern void kput_dec(unsigned v);

/* asm helpers from entry.asm */
extern void isr_syscall(void);
extern uint32_t isr_fault_table[];   /* 32 stub addresses */
void ring3_leave(void);
extern void isr_irq0(void);          /* IRQ0 preemption gate          */
extern void jmp_user(uint32_t *fr);  /* iret into a saved user frame  */

/* kernel.c: consume raw make-scancode `raw` from the PS/2 ring */
extern int kb_take_raw(uint8_t raw);
/* kernel.c: F11/F12 seen by a process's own keyboard poll (0 = none) */
extern int kb_take_hotkey(void);
/* kernel.c: clear the shared console (last process exited) */
extern void kcls(void);

static inline void outb(uint16_t port, uint8_t v)
{
    __asm__ volatile ("outb %0, %1" : : "a"(v), "Nd"(port));
}

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
    fn5 cls, set_color, getuser, getdate, readfile, spawn, procs, writefile;
    fn5 listdir, fsop, sysop;
    fn5 ticks;
    fn5 putcell, cellfill, cputs, cursor;   /* TUI primitives (appended) */
} kapi_t;
static const kapi_t *g_api;

/* register frame pushed by pushad + CPU fault frame after it:
   [0]edi [1]esi [2]ebp [3]esp [4]ebx [5]edx [6]ecx [7]eax
   [8]err [9]eip [10]cs [11]eflags [12]user_esp (PL3 frames)     */
typedef struct { uint32_t r[13]; } regs_t;

/* syscall numbers == trampoline stub order */
enum { SYS_PUTC, SYS_PUTS, SYS_GETCHAR, SYS_EXIT,
       SYS_PIXEL, SYS_FILL, SYS_TEXT, SYS_GETKEY,
       SYS_MOUSE, SYS_GETPIXEL,
       SYS_CLS, SYS_SETCOLOR, SYS_GETUSER, SYS_GETDATE, SYS_READFILE,
       SYS_SPAWN, SYS_PROCS, SYS_WRITEFILE,
       SYS_LISTDIR, SYS_FSOP, SYS_SYSOP, SYS_TICKS,
       SYS_PUTCELL, SYS_CELLFILL, SYS_CPUTS, SYS_CURSOR, SYS_COUNT };

/* ============================================================
 * Processes: 4 fixed slots, ONE address space (no CR3 switch).
 * Each slot owns a link base (the build emits a binary per slot)
 * and a stack region - all inside the user-mapped 3..5MB window.
 * ============================================================ */
#define NSLOT  4
#define NSAVE  13                       /* pushad(8) + eip cs efl esp ss */
enum { PST_FREE = 0, PST_READY, PST_FRESH, PST_STOPPED };

static const uint32_t slot_base[NSLOT]  = { 0x00300000u, 0x00320000u,
                                            0x00340000u, 0x00360000u };
static const uint32_t slot_stack[NSLOT] = { 0x00500000u, 0x004C0000u,
                                            0x00480000u, 0x00440000u };

extern uint32_t g_nxp_exit_stub;    /* defined below, set by setup_tramp */

typedef struct {
    uint32_t state;                     /* PST_*                  */
    uint32_t fr[NSAVE];                 /* saved iret frame       */
    char     name[13];                  /* program file name      */
    uint32_t cwd;                       /* per-process NovaFS cwd */
} pcb_t;
static pcb_t g_pcb[NSLOT];
static int  g_cur  = -1;                /* pcb interrupted/running */
static int  g_last = -1;                /* last scheduled, for RR  */
static volatile uint32_t g_ticks;       /* 10 ms since boot (IRQ0) */

uint32_t proc_slot_base(int slot) { return slot_base[slot]; }

uint32_t proc_ticks(void) { return g_ticks; }   /* 10 ms units */

int proc_spawn(int slot, const char *name)
{
    if (slot < 0 || slot >= NSLOT || g_pcb[slot].state != PST_FREE) return -1;
    pcb_t *p = &g_pcb[slot];
    for (int i = 0; i < NSAVE; i++) p->fr[i] = 0;
    p->fr[7]  = NXP_TRAMP_API;          /* user EAX = api table  */
    p->fr[8]  = slot_base[slot] + 4;    /* entry eip             */
    p->fr[9]  = 0x1B;                   /* user cs               */
    p->fr[10] = 0x202;                  /* eflags, IF=1: preemptible */
    *(uint32_t *)(slot_stack[slot] - 4) = g_nxp_exit_stub;
    p->fr[11] = slot_stack[slot] - 4;   /* esp: `ret` hits the exit stub */
    p->fr[12] = 0x23;                   /* user ss               */
    p->cwd = (uint32_t)fs_cwd();        /* inherit spawner's directory */
    p->state = PST_READY;
    int i = 0;
    while (name && name[i] && i < 12) { p->name[i] = name[i]; i++; }
    p->name[i] = 0;
    return slot + 1;
}

/* fill buf with "pid st name\n" lines for live processes; returns count
 * (st: r = ready/running, s = suspended) */
int proc_list(char *buf, uint32_t max)
{
    int n = 0;
    uint32_t o = 0;
    for (int i = 0; i < NSLOT; i++) {
        if (g_pcb[i].state == PST_FREE) continue;
        n++;
        if (o + 4 >= max) continue;     /* no room: count only */
        buf[o++] = (char)('1' + i);     /* pid = slot + 1 (1 digit) */
        buf[o++] = ' ';
        buf[o++] = (g_pcb[i].state == PST_STOPPED) ? 's' : 'r';
        buf[o++] = ' ';
        int j = 0;
        while (g_pcb[i].name[j] && o < max - 2) buf[o++] = g_pcb[i].name[j++];
        buf[o++] = '\n';
    }
    if (max) buf[o < max ? o : max - 1] = 0;
    return n;
}

uint32_t *proc_frame(int slot) { return g_pcb[slot].fr; }

/* round robin: next schedulable pcb after g_last (skips free+suspended) */
static int sched_pick(void)
{
    for (int k = 1; k <= NSLOT; k++) {
        int i = (g_last + k) % NSLOT;
        if (g_pcb[i].state == PST_READY || g_pcb[i].state == PST_FRESH)
            return i;
    }
    return -1;
}

void proc_kill_all(void)
{
    for (int i = 0; i < NSLOT; i++) g_pcb[i].state = PST_FREE;
    g_cur = -1;
}

int proc_any(void)
{
    for (int i = 0; i < NSLOT; i++)
        if (g_pcb[i].state != PST_FREE) return 1;
    return 0;
}

int proc_stopped_any(void)
{
    for (int i = 0; i < NSLOT; i++)
        if (g_pcb[i].state == PST_STOPPED) return 1;
    return 0;
}

void proc_resume_all(void)
{
    for (int i = 0; i < NSLOT; i++)
        if (g_pcb[i].state == PST_STOPPED) g_pcb[i].state = PST_READY;
}

int proc_next(void) { return sched_pick(); }

/* the shell (re)enters a process directly via jmp_user: mark it as the
 * running one so the first tick saves instead of rolling it back */
void proc_set_current(int slot) { g_cur = slot; g_last = slot; }

int proc_kill(int pid)
{
    if (pid < 1 || pid > NSLOT || g_pcb[pid - 1].state == PST_FREE) return -1;
    g_pcb[pid - 1].state = PST_FREE;
    return 0;
}

static void proc_stop_all(void)
{
    for (int i = 0; i < NSLOT; i++)
        if (g_pcb[i].state != PST_FREE) g_pcb[i].state = PST_STOPPED;
    g_cur = -1;
}

/* IRQ0 preemption: only user-mode frames are ever switched, so the
 * kernel is never re-entered. The stub hands us the pushad area; the
 * CPU iret frame (eip cs efl [esp ss]) sits right above it. */
void irq0_dispatch(uint32_t *f)
{
    outb(0x20, 0x20);                   /* EOI first: we may not iret */
    g_ticks++;                          /* the 10 ms wall clock */
    if ((f[9] & 3) != 3) return;        /* interrupted the kernel: skip -
                                         * hotkeys stay queued for next tick */
    if (g_cur >= 0) {                   /* snapshot current before anything */
        for (int i = 0; i < NSAVE; i++) g_pcb[g_cur].fr[i] = f[i];
        g_pcb[g_cur].state = PST_READY;
    }
    if (kb_take_raw(0x57) && proc_any()) {      /* F11: freeze all */
        proc_stop_all();
        kcls();                         /* hand the shell a clean screen */
        ring3_leave();
    }
    if (kb_take_raw(0x58) && proc_any()) {      /* F12: kill all */
        proc_kill_all();
        kcls();
        ring3_leave();
    }
    int nx = sched_pick();
    if (nx < 0) return;                 /* alone: keep running */
    for (int i = 0; i < NSAVE; i++) f[i] = g_pcb[nx].fr[i];
    g_cur = nx;
    g_last = nx;
}

uint32_t g_nxp_exit_stub;

/* runtime stub addresses, for fault diagnostics */
static uint32_t g_stub_addr[SYS_COUNT];

/* ---- accept only pointers inside the CALLING process's memory ----
 * The address space is shared, so this check IS the process isolation:
 * a syscall may touch its own slot image (+BSS), its own stack window,
 * or the shared trampoline page - never another process's memory. */
static uint32_t uptr(uint32_t p)
{
    if (p >= NXP_TRAMP_BASE && p < NXP_TRAMP_BASE + 0x1000u) return p;
    if (g_cur < 0) return 0;
    uint32_t b = slot_base[g_cur];
    if (p >= b && p < b + 0x2000u) return p;            /* image + BSS  */
    uint32_t t = slot_stack[g_cur];
    if (p >= t - 0x8000u && p < t) return p;            /* stack window */
    return 0;
}

void syscall_dispatch(regs_t *r)
{
    uint32_t ret = 0;
    if (r->r[7] >= SYS_COUNT) { r->r[7] = 0; return; }
    uint32_t *a = (uint32_t *)uptr(r->r[4]);   /* EBX = args block */
    if (!a) { r->r[7] = 0; return; }

    /* per-process cwd: every fs-touching syscall runs against the
     * caller's own directory, restored before returning */
    int saved_cwd = -1;
    if (g_cur >= 0) {
        saved_cwd = fs_cwd();
        fs_setcwd((int)g_pcb[g_cur].cwd);
    }

    switch (r->r[7]) {
    case SYS_PUTC:     g_api->putc(a[0], 0, 0, 0, 0); break;
    case SYS_PUTS:     g_api->puts(uptr(a[0]), 0, 0, 0, 0); break;
    case SYS_GETCHAR:  ret = g_api->getchar(0, 0, 0, 0, 0); break;
    case SYS_EXIT: {
        /* process death: reuse this very frame for the next process */
        if (g_cur >= 0) g_pcb[g_cur].state = PST_FREE;
        g_cur = -1;
        if (saved_cwd >= 0) fs_setcwd(saved_cwd);
        int nx = sched_pick();
        if (nx < 0) { kcls(); ring3_leave(); }   /* none left: clean shell */
        for (int i = 0; i < NSAVE; i++) r->r[i] = g_pcb[nx].fr[i];
        g_cur = nx;
        g_last = nx;
        return;                         /* frame replaced: keep its EAX */
    }
    case SYS_SPAWN:    ret = g_api->spawn(uptr(a[0]), 0, 0, 0, 0); break;
    case SYS_PROCS:    ret = g_api->procs(uptr(a[0]), a[1], 0, 0, 0); break;
    case SYS_WRITEFILE:
        ret = g_api->writefile(uptr(a[0]), uptr(a[1]), a[2], 0, 0); break;
    case SYS_LISTDIR:
        ret = g_api->listdir(uptr(a[0]), a[1], 0, 0, 0); break;
    case SYS_FSOP: {
        ret = g_api->fsop(a[0], uptr(a[1]), 0, 0, 0);
        /* op 5 = cd: make the new directory stick to this process */
        if (a[0] == 5 && ret == 0 && g_cur >= 0) g_pcb[g_cur].cwd = fs_cwd();
        break;
    }
    case SYS_SYSOP:
        ret = g_api->sysop(a[0], uptr(a[1]), 0, 0, 0); break;
    case SYS_TICKS:
        ret = g_api->ticks(0, 0, 0, 0, 0); break;
    case SYS_PIXEL:    g_api->pixel(a[0], a[1], a[2], 0, 0); break;
    case SYS_FILL:     g_api->fill_rect(a[0], a[1], a[2], a[3], a[4]); break;
    case SYS_TEXT:     g_api->text(a[0], a[1], uptr(a[2]), a[3], 0); break;
    case SYS_PUTCELL:  g_api->putcell(a[0], a[1], a[2], a[3], 0); break;
    case SYS_CELLFILL: g_api->cellfill(a[0], a[1], a[2], a[3], a[4]); break;
    case SYS_CPUTS:    g_api->cputs(a[0], a[1], uptr(a[2]), a[3], 0); break;
    case SYS_CURSOR:   g_api->cursor(a[0], 0, 0, 0, 0); break;
    case SYS_GETKEY:   ret = g_api->getkey(0, 0, 0, 0, 0); break;
    case SYS_MOUSE:
        ret = g_api->mouse(uptr(a[0]), uptr(a[1]), uptr(a[2]), 0, 0);
        break;
    case SYS_GETPIXEL: ret = g_api->get_pixel(a[0], a[1], 0, 0, 0); break;
    case SYS_CLS:      g_api->cls(0, 0, 0, 0, 0); break;
    case SYS_SETCOLOR: g_api->set_color(a[0], 0, 0, 0, 0); break;
    case SYS_GETUSER:  ret = g_api->getuser(uptr(a[0]), a[1], 0, 0, 0); break;
    case SYS_GETDATE:  ret = g_api->getdate(uptr(a[0]), a[1], 0, 0, 0); break;
    case SYS_READFILE: ret = g_api->readfile(uptr(a[0]), uptr(a[1]), a[2], 0, 0); break;
    }

    if (saved_cwd >= 0) fs_setcwd(saved_cwd);   /* back to the shell's cwd */

    /* F11/F12 pressed while a process was polling the keyboard: the
     * process consumed the raw scancode before any tick could see it,
     * so act on it right here where the full frame is in hand */
    int hk = kb_take_hotkey();
    if (hk == 0x57 && g_cur >= 0) {     /* F11: freeze all, back to shell */
        for (int i = 0; i < NSAVE; i++) g_pcb[g_cur].fr[i] = r->r[i];
        proc_stop_all();                /* everything STOPPED (cur included) */
        kcls();
        ring3_leave();                  /* never returns */
    }
    if (hk == 0x58 && g_cur >= 0) {     /* F12: kill all, back to shell */
        proc_kill_all();
        kcls();
        ring3_leave();                  /* never returns */
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
        kputs(" uesp=");  kput_hex(r->r[12]);   /* CPL3 frame carries user SS:ESP */
        if (r->r[9] >= NXP_TRAMP_BASE && r->r[9] < NXP_TRAMP_BASE + 0x1000u) {
            int s = SYS_COUNT - 1;              /* which stub region eip sits after */
            while (s > 0 && g_stub_addr[s] > r->r[9]) s--;
            kputs(" (tramp, after stub "); kput_dec((unsigned)s); kputs(")");
        }
        /* dump the first instruction bytes at the fault point — the whole
         * user area is identity-mapped, so this read cannot fault */
        if (r->r[9] >= 0x00300000u && r->r[9] < 0x00501000u) {
            static const char hx[] = "0123456789ABCDEF";
            const volatile uint8_t *ip = (const volatile uint8_t *)r->r[9];
            kputs("\n  insn:");
            for (int i = 0; i < 8; i++) {
                kput(' ');
                kput(hx[(ip[i] >> 4) & 0xF]);
                kput(hx[ip[i] & 0xF]);
            }
        }
        /* top of the user stack: the return addresses the program pushed.
         * A smashed stack (bad `ret` target) is immediately visible here. */
        if (r->r[12] >= 0x00300000u && r->r[12] < 0x00501000u) {
            volatile uint32_t *us = (volatile uint32_t *)r->r[12];
            kputs("\n  ustack:");
            for (int i = 0; i < 6; i++) { kput(' '); kput_hex(us[i]); }
        }
        kput('\n');
        /* kill the offender and keep the rest running */
        if (g_cur >= 0) { g_pcb[g_cur].state = PST_FREE; g_cur = -1; }
        int nx = sched_pick();
        if (nx >= 0) {
            proc_set_current(nx);              /* first tick must save, not
                                                * roll the new current back */
            jmp_user(g_pcb[nx].fr);            /* never returns */
        }
        kcls();                                /* none left: clean shell */
        ring3_leave();
    }

    /* kernel fault — show BSOD and halt */
    bsod_show(r, vector, cr2);
    __asm__ volatile ("cli");
    for (;;) __asm__ volatile ("hlt");
}

/* ---- trampoline page: cdecl stub -> int 0x80 -> kernel ----
 * returns the address just past the stub, so the caller lays the next
 * one down without ever duplicating the size math */
static uint8_t *emit_stub(uint8_t *p, uint32_t sysno, int nargs)
{
    *p++ = 0x53;                                        /* push ebx (callee-saved in cdecl:
                                                         * the stub must preserve it) */
    *p++ = 0xB8; *(uint32_t *)p = sysno; p += 4;        /* mov eax,sysno */
    for (int i = 0; i < nargs; i++) {
        *p++ = 0x8B; *p++ = 0x54; *p++ = 0x24;          /* mov edx,[esp+8+i*4] (+4 for
                                                         * the pushed ebx) */
        *p++ = (uint8_t)(8 + i * 4);
        *p++ = 0x89; *p++ = 0x15;                       /* mov [abs],edx  */
        *(uint32_t *)p = NXP_TRAMP_ARGS + i * 4; p += 4;
    }
    *p++ = 0xBB; *(uint32_t *)p = NXP_TRAMP_ARGS; p += 4; /* mov ebx,args */
    *p++ = 0xCD; *p++ = 0x80;                           /* int 0x80      */
    *p++ = 0x5B;                                        /* pop ebx       */
    *p++ = 0xC3;                                        /* ret           */
    return p;
}

void ring3_setup_tramp(void)
{
    static const uint8_t nargs[SYS_COUNT] = { 1,1,0,0,3,5,4,0,3,2, 0,1,2,2,3,1,2,3, 2,2,2,0,
                                              4,5,4,1 };
    uint8_t *p = (uint8_t *)NXP_TRAMP_BASE;
    uint32_t stub[SYS_COUNT];

    for (int s = 0; s < SYS_COUNT; s++) {
        stub[s] = (uint32_t)p;
        g_stub_addr[s] = stub[s];
        p = emit_stub(p, (uint32_t)s, nargs[s]);
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
    t[14] = stub[SYS_CLS];     t[15] = stub[SYS_SETCOLOR];
    t[16] = stub[SYS_GETUSER]; t[17] = stub[SYS_GETDATE];
    t[18] = stub[SYS_READFILE]; t[19] = stub[SYS_SPAWN];
    t[20] = stub[SYS_PROCS];   t[21] = stub[SYS_WRITEFILE];
    t[22] = stub[SYS_LISTDIR]; t[23] = stub[SYS_FSOP];
    t[24] = stub[SYS_SYSOP];   t[25] = stub[SYS_TICKS];
    t[26] = stub[SYS_PUTCELL]; t[27] = stub[SYS_CELLFILL];
    t[28] = stub[SYS_CPUTS];   t[29] = stub[SYS_CURSOR];
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
    /* LFB: map the PCI BAR gfx actually probed (supervisor only, PCD).
     * Two 4MB pages so an unaligned BAR still covers the 3MB console. */
    uint32_t lfb = gfx_lfb() & ~0x3FFFFFu;
    if (lfb && lfb < 0xFFC00000u) {
        pde[lfb >> 22]       = lfb | 0x93;
        pde[(lfb >> 22) + 1] = (lfb + 0x400000u) | 0x93;
    }
}

/* map a device MMIO region with 4MB pages (supervisor, RW, cache off)
 * into unused PDE slots; identity-style so VA == PA. Call AFTER paging
 * is live (ring3_init); returns phys as the VA to use, 0 if PSE is off
 * or the region would clash with an existing mapping. */
uint32_t page_map_device(uint32_t phys, uint32_t len)
{
    if (!len) return 0;
    uint32_t first = phys >> 22;
    uint32_t last  = (phys + len - 1) >> 22;
    if (last > 1023 || last - first > 1) return 0;    /* 2 PDEs is plenty */
    for (uint32_t i = first; i <= last; i++)
        if (pde[i]) return 0;                         /* slot taken */
    for (uint32_t i = first; i <= last; i++)
        pde[i] = ((uint32_t)i << 22) | 0x93;          /* P|RW|PCD|PS */
    __asm__ volatile ("mov %0, %%cr3" : : "r"(PDE_ADDR));   /* flush TLB */
    return phys;
}

/* ---- PIC remap + PIT @100Hz + IRQ0 gate: the preemption heart ----
 * Runs at the very end of ring3_init. Only IRQ0 is unmasked; keyboard,
 * mouse and disks stay polled exactly as before. */
static void pic_pit_init(void)
{
    outb(0x20, 0x11); outb(0xA0, 0x11);     /* ICW1: cascade + ICW4  */
    outb(0x21, 0x20); outb(0xA1, 0x28);     /* ICW2: vectors 32 / 40 */
    outb(0x21, 0x04); outb(0xA1, 0x02);     /* ICW3: cascade wiring  */
    outb(0x21, 0x01); outb(0xA1, 0x01);     /* ICW4: 8086 mode       */
    outb(0x21, 0xFE); outb(0xA1, 0xFF);     /* unmask IRQ0 only      */
    outb(0x43, 0x34);                       /* ch0, mode 2, binary   */
    outb(0x40, 0x9C);                       /* divisor 11932 = 100Hz */
    outb(0x40, 0x2E);
    set_gate(0x20, (void (*)(void))isr_irq0, 0x8E);
    __asm__ volatile ("sti");               /* preemption goes live  */
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

    pic_pit_init();
}
