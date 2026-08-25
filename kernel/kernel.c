/* ============================================================
 * NovaOS - 32bit kernel core
 * Features: VGA text driver, PS/2 keyboard, NovaSh shell
 * RTC CMOS read only
 * ============================================================ */
#include "stdint.h"
#include "ata.h"
#include "novafs.h"
#include "acpi.h"
#include "gfx.h"
#include "paging.h"

/* ---- Port I/O ---- */
static inline void outb(uint16_t port, uint8_t v)
{
    __asm__ volatile ("outb %0, %1" : : "a"(v), "Nd"(port));
}
static inline uint8_t inb(uint16_t port)
{
    uint8_t v;
    __asm__ volatile ("inb %1, %0" : "=a"(v) : "Nd"(port));
    return v;
}
static inline void outl(uint16_t port, uint32_t v)
{
    __asm__ volatile ("outl %0, %1" : : "a"(v), "Nd"(port));
}
static inline uint32_t inl(uint16_t port)
{
    uint32_t v;
    __asm__ volatile ("inl %1, %0" : "=a"(v) : "Nd"(port));
    return v;
}
static inline uint16_t inw(uint16_t port)
{
    uint16_t v;
    __asm__ volatile ("inw %1, %0" : "=a"(v) : "Nd"(port));
    return v;
}


/* ---- VGA text mode ---- */
#define VGA_MEM     ((volatile uint16_t*)0xB8000)
#define VGA_COLS    80
#define VGA_ROWS    25
#define C_BLACK    0x00
#define C_BLUE     0x01
#define C_GREEN    0x02
#define C_CYAN     0x03
#define C_RED      0x04
#define C_MAGENTA  0x05
#define C_BROWN    0x06
#define C_LGRAY    0x07
#define C_DGRAY    0x08
#define C_LBLUE    0x09
#define C_LGREEN   0x0A
#define C_LCYAN    0x0B
#define C_LRED     0x0C
#define C_LMAGENTA 0x0D
#define C_YELLOW   0x0E
#define C_WHITE    0x0F

static uint8_t vga_fg = C_WHITE;
static uint8_t vga_bg = C_BLACK;

static inline uint8_t vga_attr(void)
{
    return (uint8_t)((vga_bg << 4) | vga_fg);
}

static int  vga_row = 0;
static int  vga_col = 0;

static inline void vga_move_cursor(void)
{
    if (gfx_active()) { gfx_move_cursor(); return; }
    uint16_t pos = (uint16_t)(vga_row * VGA_COLS + vga_col);
    outb(0x3D4, 0x0F); outb(0x3D5, (uint8_t)(pos & 0xFF));
    outb(0x3D4, 0x0E); outb(0x3D5, (uint8_t)(pos >> 8));
}

static inline void vga_putc(char c)
{
    if (gfx_active()) { gfx_putc(c); return; }
    uint8_t a = vga_attr();
    if (c == '\n') {
        vga_col = 0;
        vga_row++;
    } else if (c == '\r') {
        vga_col = 0;
    } else if (c == '\b') {
        if (vga_col > 0) {
            vga_col--;
            VGA_MEM[vga_row * VGA_COLS + vga_col] = ((uint16_t)a << 8) | ' ';
        }
    } else {
        VGA_MEM[vga_row * VGA_COLS + vga_col] = ((uint16_t)a << 8) | (uint8_t)c;
        vga_col++;
        if (vga_col >= VGA_COLS) {
            vga_col = 0;
            vga_row++;
        }
    }

    if (vga_row >= VGA_ROWS) {
        for (int y = 0; y < VGA_ROWS - 1; y++) {
            for (int x = 0; x < VGA_COLS; x++) {
                VGA_MEM[y * VGA_COLS + x] = VGA_MEM[(y + 1) * VGA_COLS + x];
            }
        }
        for (int x = 0; x < VGA_COLS; x++) {
            VGA_MEM[(VGA_ROWS - 1) * VGA_COLS + x] = ((uint16_t)a << 8) | ' ';
        }
        vga_row = VGA_ROWS - 1;
        vga_col = 0;
    }
    vga_move_cursor();
}

static void vga_puts(const char *s)
{
    while (*s) vga_putc(*s++);
}

static void vga_clear(void)
{
    if (gfx_active()) { gfx_clear(); return; }
    uint8_t a = vga_attr();
    for (int y = 0; y < VGA_ROWS; y++) {
        for (int x = 0; x < VGA_COLS; x++) {
            VGA_MEM[y * VGA_COLS + x] = ((uint16_t)a << 8) | ' ';
        }
    }
    vga_row = 0;
    vga_col = 0;
    vga_move_cursor();
}

static void set_color(uint8_t fg)
{
    vga_fg = fg;
    vga_bg = C_BLACK;
    if (gfx_active()) gfx_set_colors(fg, C_BLACK);
}
static void reset_color(void)
{
    vga_fg = C_WHITE;
    vga_bg = C_BLACK;
}

/* ---- Serial (COM1) ---- */
#define COM1 0x3F8
static inline void io_wait(void) { outb(0x80, 0); }

static void serial_init(void)
{
    outb(COM1 + 1, 0x00); outb(COM1 + 3, 0x80);
    outb(COM1 + 0, 0x01); outb(COM1 + 1, 0x00);   /* divisor 1 = 115200 baud */
    outb(COM1 + 3, 0x03); outb(COM1 + 2, 0xC7); outb(COM1 + 4, 0x0B);
}

static void serial_putc(char c)
{
    int timeout = 100000;
    while (!(inb(COM1 + 5) & 0x20) && --timeout) ;
    outb(COM1, (uint8_t)c);
}

static void serial_puts(const char *s)
{
    while (*s) serial_putc(*s++);
}

void kput(char c)   { vga_putc(c);    serial_putc(c); }
void kputs(const char *s) { vga_puts(s); serial_puts(s); }

/* ---- Integer helpers ---- */
void kput_dec(unsigned v)
{
    char buf[12]; int i = 0;
    if (v == 0) { kput('0'); return; }
    while (v) { buf[i++] = '0' + (v % 10); v /= 10; }
    while (i--) kput(buf[i]);
}

void kput_hex(unsigned v)
{
    static const char *hex = "0123456789ABCDEF";
    kputs("0x");
    for (int i = 28; i >= 0; i -= 4)
        kput(hex[(v >> i) & 0xF]);
}

/* ---- String helpers ---- */
static int str_eq(const char *a, const char *b)
{
    while(*a && *b){ if(*a!=*b) return 0; a++; b++; }
    return *a==*b;
}
static int str_len(const char *s)
{
    int n=0; while(s[n])n++; return n;
}

/* ---- PS/2 keyboard ---- */
#define KB_DATA 0x60
#define KB_STAT 0x64
static const char scancode_map[128] = {
    0,0,'1','2','3','4','5','6','7','8','9','0','-','=', '\b',0,
    'q','w','e','r','t','y','u','i','o','p','[',']','\n',0,
    'a','s','d','f','g','h','j','k','l',';','\'','`',0,'\\',
    'z','x','c','v','b','n','m',',','.','/',0,'*',0,' ',0,
};
static const char shift_map[128] = {
    0,0,'!','@','#','$','%','^','&','*','(',')','_','+','\b',0,
    'Q','W','E','R','T','Y','U','I','O','P','{','}','\n',0,
    'A','S','D','F','G','H','J','K','L',':','"','~',0,'|',
    'Z','X','C','V','B','N','M','<','>','?',0,'*',0,' ',0,
};
static int kb_shift = 0;
static int kb_caps  = 0;
static int kb_ext   = 0;   /* saw an 0xE0 extended prefix */

/* extended-key codes surfaced to applications */
#define K_LEFT  0x11
#define K_RIGHT 0x12
#define K_UP    0x13
#define K_DOWN  0x14

static char kb_decode(uint8_t sc);
static void ps2_drain(void);
static int  kbuf_pop(void);

static char kb_decode(uint8_t sc)
{
    if (kb_ext) {                       /* second byte of an extended key */
        kb_ext = 0;
        if (sc & 0x80) return 0;        /* extended break */
        if (sc == 0x4B) return K_LEFT;
        if (sc == 0x4D) return K_RIGHT;
        if (sc == 0x48) return K_UP;
        if (sc == 0x50) return K_DOWN;
        return 0;
    }
    if (sc == 0xE0) { kb_ext = 1; return 0; }
    if (sc & 0x80) {
        sc &= 0x7F;
        if (sc == 0x2A || sc == 0x36) kb_shift = 0;
        return 0;
    }
    if (sc == 0x2A || sc == 0x36) { kb_shift = 1; return 0; }
    if (sc == 0x3A) { kb_caps = !kb_caps; return 0; }
    if (sc >= 128) return 0;
    char c = kb_shift ? shift_map[sc] : scancode_map[sc];
    if (c >= 'a' && c <= 'z' && kb_caps && !kb_shift) c -= 32;
    if (c >= 'A' && c <= 'Z' && kb_caps && kb_shift)  c += 32;
    return c;
}

static char kb_getc(void)
{
    for (;;) {
        ps2_drain();
        int v = kbuf_pop();
        if (v < 0) continue;                 /* blocking: keep waiting */
        char c = kb_decode((uint8_t)v);
        if (c) return c;
    }
}

static char kb_read(void)
{
    char c; while (!(c = kb_getc())) ; return c;
}

/* ============================================================
 * PS/2 mouse (8042 auxiliary port, polled)
 * 3-byte packets [flags dx dy]; status bit5 tags aux bytes.
 * ============================================================ */
#define AUX_TAG 0x20

static uint8_t mpkt[3];
static int     mpkt_state;
static int     mouse_dx, mouse_dy;    /* accumulated deltas */
static int     mouse_btns;            /* 1=L 2=R 4=M */
static int     mouse_pkts;            /* packets pending for the app */

static void mouse_feed(uint8_t b)
{
    if (mpkt_state == 0 && !(b & 0x08)) return;   /* resync on flags byte */
    mpkt[mpkt_state++] = b;
    if (mpkt_state == 3) {
        mpkt_state = 0;
        int dx = mpkt[1], dy = mpkt[2];
        if (mpkt[0] & 0x10) dx -= 256;
        if (mpkt[0] & 0x20) dy -= 256;
        mouse_dx += dx;
        mouse_dy -= dy;                          /* PS/2 +y is up, screen +y is down */
        mouse_btns = mpkt[0] & 7;
        mouse_pkts++;
    }
}

/* keyboard scancode ring: both pollers share port 0x60 */
#define KBUF 32
static uint8_t kbuf[KBUF];
static int     khead, ktail;
static void kbuf_push(uint8_t sc)
{
    int n = (ktail + 1) % KBUF;
    if (n != khead) { kbuf[ktail] = sc; ktail = n; }
}
static int kbuf_pop(void)
{
    if (khead == ktail) return -1;
    int v = kbuf[khead];
    khead = (khead + 1) % KBUF;
    return v;
}

/* move all pending controller bytes into mouse/kbd buffers */
static void ps2_drain(void)
{
    for (int guard = 0; guard < 64; guard++) {
        uint8_t st = inb(KB_STAT);
        if (!(st & 1)) break;
        uint8_t b = inb(KB_DATA);
        if (st & AUX_TAG) mouse_feed(b);
        else              kbuf_push(b);
    }
}

static int mouse_wait(int want_out)
{
    int t = 100000;
    while (t--) {
        uint8_t s = inb(KB_STAT);
        if (want_out ? !(s & 2) : (s & 1)) return 0;
    }
    return -1;
}
static int mouse_cmd(uint8_t b)
{
    if (mouse_wait(1) < 0) return -1;  outb(0x64, 0xD4);
    if (mouse_wait(1) < 0) return -1;  outb(0x60, b);
    if (mouse_wait(0) < 0) return -1;
    uint8_t st = inb(KB_STAT);
    uint8_t ack = inb(KB_DATA);
    (void)st;                       /* the 0xFA ack is not packet data - never feed it */
    return (ack == 0xFA) ? 0 : -1;
}
static int mouse_init(void)
{
    if (mouse_wait(1) < 0) return -1;  outb(0x64, 0xA8);      /* enable aux */
    if (mouse_wait(1) < 0) return -1;  outb(0x64, 0x20);      /* read config */
    if (mouse_wait(0) < 0) return -1;
    uint8_t st = inb(KB_STAT); uint8_t cfg = inb(KB_DATA);
    (void)st;                                                    /* config is not packet data */
    cfg &= (uint8_t)~0x20;                                       /* aux clock on */
    if (mouse_wait(1) < 0) return -1;  outb(0x64, 0x60);
    if (mouse_wait(1) < 0) return -1;  outb(0x60, cfg);
    if (mouse_cmd(0xF6) < 0) return -1;                          /* defaults */
    if (mouse_cmd(0xF4) < 0) return -1;                          /* streaming */
    mpkt_state = 0;                                              /* start on a packet boundary */
    return 0;
}

/* non-blocking: -1 if no key is waiting (skips internal codes) */
static int kb_poll(void)
{
    for (;;) {
        ps2_drain();
        int v = kbuf_pop();
        if (v < 0) return -1;
        char c = kb_decode((uint8_t)v);
        if (c) return c;
    }
}

/* ---- CMOS/RTC READ ONLY ---- */
#define CMOS_ADDR 0x70
#define CMOS_DATA 0x71

static uint8_t bcd_to_bin(uint8_t bcd)
{
    return (uint8_t)(((bcd >> 4) & 0x0F) * 10 + (bcd & 0x0F));
}

static uint8_t cmos_read(uint8_t reg)
{
    outb(CMOS_ADDR, reg);
    io_wait(); io_wait(); io_wait();
    return inb(CMOS_DATA);
}

static void cmd_date(void)
{
    uint8_t sec1, min1, hour_raw1, day1, mon1, y_raw1, dow1;
    uint8_t sec2, min2, hour_raw2, day2, mon2, y_raw2, dow2;

retry_rtc:
retry1:
    if (cmos_read(0x0A) & 0x80)
        goto retry1;

    sec1      = cmos_read(0x00);
    min1      = cmos_read(0x02);
    hour_raw1 = cmos_read(0x04);
    day1      = cmos_read(0x07);
    mon1      = cmos_read(0x08);
    y_raw1    = cmos_read(0x09);
    dow1      = cmos_read(0x06);

retry2:
    if (cmos_read(0x0A) & 0x80)
        goto retry2;

    sec2      = cmos_read(0x00);
    min2      = cmos_read(0x02);
    hour_raw2 = cmos_read(0x04);
    day2      = cmos_read(0x07);
    mon2      = cmos_read(0x08);
    y_raw2    = cmos_read(0x09);
    dow2      = cmos_read(0x06);

    if (sec1 != sec2 || min1 != min2 || hour_raw1 != hour_raw2 ||
        day1 != day2 || mon1 != mon2 || y_raw1 != y_raw2 || dow1 != dow2)
    {
        goto retry_rtc;
    }

    uint8_t sec     = sec1;
    uint8_t min     = min1;
    uint8_t hour    = hour_raw1;
    uint8_t day     = day1;
    uint8_t mon     = mon1;
    uint8_t y_raw   = y_raw1;
    uint8_t dow     = dow1;

    kputs("REG00(sec)="); kput_hex(sec);
    kputs("  REG02(min)="); kput_hex(min);
    kputs("\n");

    kputs("REG04(hour)="); kput_hex(hour);
    kputs("  REG06(dow)="); kput_hex(dow);
    kputs("\n");

    kputs("REG07(day)="); kput_hex(day);
    kputs("  REG08(mon)="); kput_hex(mon);
    kputs("\n");

    kputs("REG09(year)="); kput_hex(y_raw);
    kputs("\n");

    /* reg B tells us the RTC data format: bit1 = 24h, bit2 = binary */
    uint8_t regb = cmos_read(0x0B);
    if(!(regb & 0x04)){                     /* BCD mode */
        sec   = bcd_to_bin(sec);
        min   = bcd_to_bin(min);
        day   = bcd_to_bin(day);
        mon   = bcd_to_bin(mon);
        y_raw = bcd_to_bin(y_raw);
        hour  = bcd_to_bin(hour & 0x7F);
    }else{
        hour &= 0x7F;                       /* binary: just drop the PM flag */
    }
    if(!(regb & 0x02)){                     /* 12-hour mode */
        int pm = hour_raw1 & 0x80;
        if(pm  && hour != 12) hour = (uint8_t)(hour + 12);   /* PM */
        if(!pm && hour == 12) hour = 0;                      /* 12 AM */
    }

    uint16_t year;

    if (y_raw < 80)
        year = 2000 + y_raw;
    else
        year = 1900 + y_raw;

    static const char *wday[] = {
        "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"
    };

    const char *dn = (dow >= 1 && dow <= 7) ? wday[dow - 1] : "???";

    kputs("Date: ");
    kputs(dn);
    kput(' ');
    kput_dec(mon);
    kput('/');
    kput_dec(day);
    kput('/');
    kput_dec(year);
    kputs("  Time: ");

    if (hour < 10)
        kput('0');
    kput_dec(hour);
    kput(':');

    if (min < 10)
        kput('0');
    kput_dec(min);
    kput(':');

    if (sec < 10)
        kput('0');
    kput_dec(sec);

    kput('\n');
}

/* ---- Shell ---- */
#define CMD_MAX 128
#define TXT_MAX  (6*512)
static char cmdline[CMD_MAX];
static int  cmdlen = 0;
static char textbuf[TXT_MAX];
static char cwdbuf[128];

static void shell_prompt(void)
{
    fs_getcwd(cwdbuf, sizeof(cwdbuf));
    set_color(C_LCYAN); vga_puts("\nnovaos:");
    set_color(C_YELLOW); vga_puts(cwdbuf);
    set_color(C_DGRAY);  vga_puts("#");
    reset_color(); vga_putc(' ');
    serial_puts("\nnovaos:"); serial_puts(cwdbuf); serial_puts("# ");
}

static void cmd_help(void)
{
    set_color(C_DGRAY); vga_puts(" --- commands ---\n"); reset_color();
    set_color(C_LCYAN); vga_puts("  help    "); reset_color(); kputs("show this help\n");
    set_color(C_LCYAN); vga_puts("  ver     "); reset_color(); kputs("kernel version\n");
    set_color(C_LCYAN); vga_puts("  about   "); reset_color(); kputs("about NovaOS\n");
    set_color(C_LCYAN); vga_puts("  echo X  "); reset_color(); kputs("print text\n");
    set_color(C_LCYAN); vga_puts("  cls     "); reset_color(); kputs("clear screen\n");
    set_color(C_LCYAN); vga_puts("  date    "); reset_color(); kputs("date and time\n");
    set_color(C_LCYAN); vga_puts("  mem     "); reset_color(); kputs("memory layout\n");
    set_color(C_LCYAN); vga_puts("  acpi    "); reset_color(); kputs("ACPI tables + \\_S5 info\n");
    set_color(C_LCYAN); vga_puts("  ls      "); reset_color(); kputs("list current directory\n");
    set_color(C_LCYAN); vga_puts("  cd D    "); reset_color(); kputs("change directory (.. / /)\n");
    set_color(C_LCYAN); vga_puts("  mkdir D "); reset_color(); kputs("create directory\n");
    set_color(C_LCYAN); vga_puts("  rmdir D "); reset_color(); kputs("remove directory (must be empty)\n");
    set_color(C_LCYAN); vga_puts("  rd D    "); reset_color(); kputs("force delete dir + contents\n");
    set_color(C_LCYAN); vga_puts("  cat F   "); reset_color(); kputs("show file\n");
    set_color(C_LCYAN); vga_puts("  write F "); reset_color(); kputs("create file (end with .)\n");
    set_color(C_LCYAN); vga_puts("  rm F    "); reset_color(); kputs("delete file\n");
    set_color(C_LCYAN); vga_puts("  run F   "); reset_color(); kputs("execute a .nxp program\n");
    set_color(C_LCYAN); vga_puts("  mkdemo  "); reset_color(); kputs("create demo.nxp sample program\n");
    set_color(C_LCYAN); vga_puts("  format  "); reset_color(); kputs("format NovaFS\n");
    set_color(C_LCYAN); vga_puts("  fsinfo  "); reset_color(); kputs("filesystem info\n");
    set_color(C_LCYAN); vga_puts("  reboot  "); reset_color(); kputs("restart\n");
    set_color(C_LCYAN); vga_puts("  shutdown"); reset_color(); kputs("  power off\n");
    set_color(C_LCYAN); vga_puts("  halt    "); reset_color(); kputs("halt cpu\n");
}

static void cmd_ver(void)
{
    set_color(C_LCYAN); vga_puts("novaos"); reset_color();
    kputs(" v0.2.1  (32bit)  ");
    set_color(C_DGRAY); vga_puts(__DATE__); reset_color(); kput('\n');
}

static void cmd_about(void)
{
    set_color(C_LCYAN); vga_puts("novaos"); reset_color();
    kputs(" - tiny 32bit operating system\n");
    set_color(C_DGRAY); vga_puts("  boot: asm  |  kernel: c  |  shell: novash\n"); reset_color();
}

static void cmd_echo(const char *args) { if (args) kputs(args); kput('\n'); }
static void cmd_cls(void) { vga_clear(); }

static void cmd_mem(void)
{
    kputs("Memory map (static, hardcoded):\n");
    kputs("  Kernel image @ "); kput_hex(0x00100000); kput('\n');
    kputs("  Stack top    @ "); kput_hex(0x00200000); kput('\n');
    kputs("  VGA buffer   @ "); kput_hex(0x000B8000); kput('\n');
}

static void cmd_reboot(void)
{
    kputs("Rebooting...\n");
    serial_puts("REBOOT_REQUEST\n");
    /* keyboard controller reset */
    { uint8_t v; do { v = inb(0x64); } while (v & 0x02); outb(0x64, 0xFE); }
    __asm__ volatile ("cli"); while (1) { __asm__ volatile ("hlt"); }
}

static void cmd_halt(void)
{
    kputs("System halted. Goodbye.\n");
    __asm__ volatile ("cli"); while (1) { __asm__ volatile ("hlt"); }
}

static inline void outw(uint16_t port, uint16_t v)
{
    __asm__ volatile ("outw %0, %1" : : "a"(v), "Nd"(port));
}


static void cmd_shutdown(void)
{
    kputs("[shutdown] start\n");
    serial_puts("SHUTDOWN_REQUEST\n");

    if (g_acpi.fadt) {
        kputs("[shutdown] ACPI: PM1a="); kput_hex(g_acpi.pm1a_cnt);
        kputs(" PM1b="); kput_hex(g_acpi.pm1b_cnt);
        if (g_acpi.s5_found) {
            kputs("  _S5: a="); kput_dec(g_acpi.slp_typa);
            kputs(" b="); kput_dec(g_acpi.slp_typb); kput('\n');
        } else {
            kputs("  _S5 not found, assuming SLP_TYP=0\n");
        }
        acpi_poweroff();
        /* poweroff is async: if it works we never execute past the hlt loop */
        __asm__ volatile ("cli");
        for (;;) { __asm__ volatile ("hlt"); }
    }

    /* no ACPI tables at all: QEMU-specific fallbacks */
    kputs("[shutdown] ACPI tables missing, trying PIIX4 default (0x604)...\n");
    outw(0x604, 0x2000);
    kputs("[shutdown] trying QEMU debug-exit port 0x501...\n");
    outw(0x501, 0x0000);
    kputs("[shutdown] FAILED: qemu did not exit\n");

    __asm__ volatile ("cli");
    while (1) { __asm__ volatile ("hlt"); }
}

static void cmd_acpi(void)
{
    if (!g_acpi.rsdp) { kputs("ACPI: RSDP not found.\n"); return; }
    kputs("ACPI tables:\n");
    kputs("  RSDP @ "); kput_hex(g_acpi.rsdp);
    kputs("  ("); kputs(g_acpi.xsdt ? "XSDT" : "RSDT"); kputs(" @ ");
    kput_hex(g_acpi.sdt); kputs(")\n");
    kputs("  FADT @ "); kput_hex(g_acpi.fadt);
    kputs("  DSDT @ "); kput_hex(g_acpi.dsdt); kput('\n');
    kputs("  PM1a_CNT = "); kput_hex(g_acpi.pm1a_cnt);
    kputs("   PM1b_CNT = "); kput_hex(g_acpi.pm1b_cnt); kput('\n');
    if (g_acpi.s5_found) {
        kputs("  \\_S5 package: SLP_TYPa = "); kput_dec(g_acpi.slp_typa);
        kputs(", SLP_TYPb = "); kput_dec(g_acpi.slp_typb); kput('\n');
    } else {
        kputs("  \\_S5 not found (shutdown falls back to SLP_TYP=0)\n");
    }
    if (g_acpi.aml_err != 0xFFFFFFFFu) {
        kputs("  AML walk stopped early @ table offset 0x");
        kput_hex(g_acpi.aml_err); kputs(" (partial parse)\n");
    } else {
        kputs("  AML walk: clean\n");
    }
}




/* ============================================================
 * .nxp program loader
 *   format : flat binary, "NXP\x01" magic (4B) + code
 *   load   : 0x00300000, entry = base+4
 *   entry  : EAX = pointer to nxp_api_t below, fresh stack,
 *            returns with `ret` or api->exit()
 * ============================================================ */
#define NXP_BASE   0x00300000u
#define NXP_STACK  0x00500000u

typedef struct {
    uint32_t magic;                 /* 'NXP1' */
    uint32_t version;               /* 1 */
    void (*putc)(char);
    void (*puts)(const char *);
    int  (*getchar)(void);
    void (*exit)(void);
    uint32_t scr_w, scr_h;          /* 0,0 when graphics is unavailable */
    void (*pixel)(uint32_t x, uint32_t y, uint32_t rgb);
    void (*fill_rect)(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t rgb);
    void (*text)(uint32_t x, uint32_t y, const char *s, uint32_t rgb);
    int  (*getkey)(void);           /* non-blocking: -1 = no key */
    int  (*mouse)(int *dx, int *dy, int *btns);  /* deltas since last call */
    uint32_t (*get_pixel)(uint32_t x, uint32_t y);   /* for XOR cursors */
} nxp_api_t;

static void nxp_api_putc(char c)              { kput(c); }
static void nxp_api_puts(const char *s)       { kputs(s); }
static int  nxp_api_getchar(void)             { return kb_read(); }
static void nxp_api_exit(void)                { }   /* Ring3 exit goes through the syscall stub */
static void nxp_api_pixel(uint32_t x, uint32_t y, uint32_t rgb) { gfx_pixel((int)x, (int)y, rgb); }
static void nxp_api_fill(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t rgb)
{
    for (uint32_t r = 0; r < h; r++)
        for (uint32_t c = 0; c < w; c++)
            gfx_pixel((int)(x + c), (int)(y + r), rgb);
}
static void nxp_api_text(uint32_t x, uint32_t y, const char *s, uint32_t rgb)
{
    if (gfx_active()) gfx_text((int)x, (int)y, s, rgb);
    else kputs(s);
}
static int nxp_api_getkey(void) { return kb_poll(); }
static uint32_t nxp_api_getpixel(uint32_t x, uint32_t y)
{
    return gfx_active() ? gfx_pixel_get((int)x, (int)y) : 0;
}
static int nxp_api_mouse(int *dx, int *dy, int *btns)
{
    ps2_drain();
    int n = mouse_pkts;
    if (dx)   *dx   = mouse_dx;
    if (dy)   *dy   = mouse_dy;
    if (btns) *btns = mouse_btns;
    mouse_dx = mouse_dy = mouse_pkts = 0;
    return n;
}

static nxp_api_t nxp_api = {
    0x3150584Eu, 1,
    nxp_api_putc, nxp_api_puts, nxp_api_getchar, nxp_api_exit,
    0, 0,
    nxp_api_pixel, nxp_api_fill, nxp_api_text,
    nxp_api_getkey,
    nxp_api_mouse,
    nxp_api_getpixel
};

static void cmd_run(const char *name)
{
    if(!name || !*name){ kputs("usage: run <file.nxp>\n"); return; }
    if(!fs_is_ready()){ kputs("NovaFS not formatted. Use 'format'.\n"); return; }
    int sz = fs_size(name);
    if(sz < 5){ kputs("run: no such file\n"); return; }
    if(sz > TXT_MAX) sz = TXT_MAX;
    for(int i=0;i<TXT_MAX;i++) textbuf[i]=0;
    fs_read(name, (uint8_t*)textbuf, TXT_MAX);
    if(textbuf[0]!='N' || textbuf[1]!='X' || textbuf[2]!='P' || textbuf[3]!=1){
        kputs("run: not a .nxp program (bad magic)\n"); return;
    }
    uint8_t *dst = (uint8_t *)NXP_BASE;
    for(int i=0;i<sz;i++) dst[i] = (uint8_t)textbuf[i];
    for(int i=sz;i<8192;i++) dst[i] = 0;   /* slack + BSS */
    kputs("[nxp] running "); kputs(name); kputs(" in ring3...\n");
    ring3_setup_tramp();
    uint32_t *sp = (uint32_t *)NXP_STACK;
    *(--sp) = g_nxp_exit_stub;             /* program `ret` -> exit stub */
    ring3_enter(NXP_BASE + 4, (uint32_t)sp, NXP_TRAMP_API);
    kputs("[nxp] program returned\n");
}

/* built-in demo program (hand-assembled, linked at 0x300000):
 * puts("Hello from demo.nxp!\n"); fill_rect(300,180,400,48,orange);
 * text(300,200,"true-color pixels from a user program",white);
 * getchar(); exit(); */
static const uint8_t nxp_demo[] = {
    0x4E,0x58,0x50,0x01,
    0x53,
    0x8B,0xD8,
    0x68,0x4E,0x00,0x30,0x00,
    0xFF,0x53,0x0C,
    0x83,0xC4,0x04,
    0x68,0x00,0x80,0xFF,0x00,
    0x6A,0x30,
    0x68,0x90,0x01,0x00,0x00,
    0x68,0xB4,0x00,0x00,0x00,
    0x68,0x2C,0x01,0x00,0x00,
    0xFF,0x53,0x24,
    0x83,0xC4,0x14,
    0x68,0x00,0xFF,0xFF,0xFF,
    0x68,0x64,0x00,0x30,0x00,
    0x68,0xC8,0x00,0x00,0x00,
    0x68,0x2C,0x01,0x00,0x00,
    0xFF,0x53,0x28,
    0x83,0xC4,0x10,
    0xFF,0x53,0x10,
    0xFF,0x53,0x14,
    'H','e','l','l','o',' ','f','r','o','m',' ','d','e','m','o','.','n','x','p','!','\n',0,
    't','r','u','e','-','c','o','l','o','r',' ','p','i','x','e','l','s',' ','f','r','o','m',' ','a',' ','u','s','e','r',' ','p','r','o','g','r','a','m',0,
};

static void cmd_mkdemo(void)
{
    if(!fs_is_ready()){ kputs("NovaFS not formatted. Use 'format'.\n"); return; }
    int w = fs_write("demo.nxp", nxp_demo, (uint32_t)sizeof(nxp_demo));
    if(w < 0){ kputs("mkdemo: write failed\n"); return; }
    kputs("demo.nxp created ("); kput_dec((unsigned)w); kputs(" bytes). Try: run demo.nxp\n");
}

/* ---- Filesystem commands ---- */
static int g_file_count;



static void ls_cb(const char *name, int type, uint32_t size)
{
    g_file_count++;
    if(type == T_DIR){
        set_color(C_LBLUE);
        kputs("  [DIR] ");
    }else{
        set_color(C_LGRAY);
        kputs("  [FILE]");
    }
    reset_color();
    kput(' '); kputs(name);
    if(type == T_FILE){ kputs("   ("); kput_dec(size); kputs(" B)"); }
    kput('\n');
}

static void count_cb(const char *name, int type, uint32_t size)
{
    (void)name; (void)type; (void)size;
    g_file_count++;
}

static void cmd_fsinfo(void)
{
    if(!fs_is_ready()){ kputs("NovaFS not formatted. Use 'format'.\n"); return; }
    kputs("NovaFS status:\n");
    kputs("  max inodes  : 256\n");
    kputs("  max blocks  : 32768 (512B each)\n");
    kputs("  max file    : 3072 B (6 direct blocks)\n");
    fs_getcwd(cwdbuf, sizeof(cwdbuf));
    kputs("  current dir : "); kputs(cwdbuf); kput('\n');
    g_file_count = 0;
    fs_list(count_cb);
    kputs("  entries here: "); kput_dec((unsigned)g_file_count); kput('\n');
}

static void cmd_format(void)
{
    kputs("Formatting NovaFS... ");
    if(fs_format() < 0){ kputs("FAILED\n"); return; }
    kputs("done. (root ready)\n");
}

static void cmd_dir(void)
{
    if(!fs_is_ready()){ kputs("NovaFS not formatted. Use 'format'.\n"); return; }
    g_file_count = 0; fs_list(ls_cb);
    kputs("  "); kput_dec((unsigned)g_file_count); kputs(" item(s)\n");
}

static void cmd_cd(const char *name)
{
    if(!name || !*name){ kputs("usage: cd <dir | .. | />\n"); return; }
    if(!fs_is_ready()){ kputs("NovaFS not formatted.\n"); return; }
    int r = fs_cd(name);
    if(r < 0) kputs("cd: no such directory\n");
}

static void cmd_mkdir(const char *name)
{
    if(!name || !*name){ kputs("usage: mkdir <dir>\n"); return; }
    if(!fs_is_ready()){ kputs("NovaFS not formatted.\n"); return; }
    int r = fs_mkdir(name);
    if(r < 0) kputs("mkdir: failed (exists? name too long?)\n");
}

static void cmd_rmdir(const char *name)
{
    if(!name || !*name){ kputs("usage: rmdir <dir>\n"); return; }
    if(!fs_is_ready()){ kputs("NovaFS not formatted.\n"); return; }
    int r = fs_rmdir(name);
    if(r == -1) kputs("rmdir: no such directory\n");
    else if(r == -2) kputs("rmdir: directory not empty\n");
}

static void cmd_rd(const char *name)
{
    if(!name || !*name){ kputs("usage: rd <dir>   (force delete, even non-empty)\n"); return; }
    if(!fs_is_ready()){ kputs("NovaFS not formatted.\n"); return; }
    int r = fs_rmtree(name);
    if(r == -1)      kputs("rd: no such directory\n");
    else if(r == -2) kputs("rd: not a directory (use rm for files)\n");
    else if(r == -3) kputs("rd: cannot delete cwd or its ancestor\n");
    else             kputs("Directory removed.\n");
}

static void cmd_cat(const char *name)
{
    if(!name || !*name){ kputs("usage: cat <file>\n"); return; }
    if(!fs_is_ready()){ kputs("NovaFS not formatted. Use 'format'.\n"); return; }
    int sz = fs_size(name);
    if(sz < 0){ kputs("No such file\n"); return; }
    if(sz > TXT_MAX) sz = TXT_MAX;
    for(int i=0;i<TXT_MAX;i++) textbuf[i]=0;
    fs_read(name, (uint8_t*)textbuf, TXT_MAX);
    for(int i=0;i<sz;i++) kput(textbuf[i]);
    if(sz>0 && textbuf[sz-1]!='\n') kput('\n');
}

static void cmd_write(const char *name)
{
    if(!name || !*name){ kputs("usage: write <file>\n"); return; }
    if(!fs_is_ready()){ kputs("NovaFS not formatted. Use 'format'.\n"); return; }
    kputs("Enter text. End with a single '.' on its own line:\n");
    int len = 0;
    for(;;){
        int ll = 0; char line[CMD_MAX];
        kputs("> ");
        for(;;){
            char c = kb_read();
            if(c == '\n'){ kput('\n'); break; }
            else if(c == '\b'){ if(ll>0){ ll--; kput('\b'); } }
            else if(ll < CMD_MAX-1){ line[ll++]=c; kput(c); }
        }
        line[ll] = 0;
        if(ll == 1 && line[0]=='.') break;
        if(len + ll + 1 >= TXT_MAX){ kputs("(buffer full)\n"); break; }
        for(int i=0;i<ll;i++) textbuf[len++] = line[i];
        textbuf[len++] = '\n';
    }
    int w = fs_write(name, (uint8_t*)textbuf, (uint32_t)len);
    if(w < 0){ kputs("Write failed\n"); return; }
    kputs("Saved "); kput_dec((unsigned)w); kputs(" bytes.\n");
}

static void cmd_rm(const char *name)
{
    if(!name || !*name){ kputs("usage: rm <file>\n"); return; }
    if(!fs_is_ready()){ kputs("NovaFS not formatted.\n"); return; }
    if(fs_remove(name) < 0) kputs("rm: failed (is it a directory? use rmdir)\n");
    else kputs("Deleted.\n");
}

static void process_cmd(void) {
    cmdline[cmdlen] = 0;
    char *args = 0;
    int i = 0;
    while (cmdline[i] == ' ') i++;
    int cmdstart = i;
    while (cmdline[i] && cmdline[i] != ' ') i++;
    if (cmdline[i] == ' ') {
        cmdline[i] = 0;
        args = &cmdline[i + 1];
        while (*args == ' ') args++;
    }
    char *cmd = &cmdline[cmdstart];
    if (cmdlen == 0 || str_len(cmd) == 0) return;
    if (str_eq(cmd, "help"))      cmd_help();
    else if (str_eq(cmd, "ver"))  cmd_ver();
    else if (str_eq(cmd, "about"))cmd_about();
    else if (str_eq(cmd, "echo")) cmd_echo(args);
    else if (str_eq(cmd, "cls"))  cmd_cls();
    else if (str_eq(cmd, "date")) cmd_date();
    else if (str_eq(cmd, "mem"))  cmd_mem();
    else if (str_eq(cmd, "acpi")) cmd_acpi();
    else if (str_eq(cmd, "reboot"))cmd_reboot();
    else if (str_eq(cmd, "shutdown"))cmd_shutdown();
    else if (str_eq(cmd, "halt")) cmd_halt();
    else if (str_eq(cmd, "fsinfo"))cmd_fsinfo();
    else if (str_eq(cmd, "format"))cmd_format();
    else if (str_eq(cmd, "ls"))    cmd_dir();
    else if (str_eq(cmd, "cd"))    cmd_cd(args);
    else if (str_eq(cmd, "mkdir")) cmd_mkdir(args);
    else if (str_eq(cmd, "rmdir")) cmd_rmdir(args);
    else if (str_eq(cmd, "rd"))    cmd_rd(args);
    else if (str_eq(cmd, "cat"))   cmd_cat(args);
    else if (str_eq(cmd, "write")) cmd_write(args);
    else if (str_eq(cmd, "rm"))    cmd_rm(args);
    else if (str_eq(cmd, "run"))   cmd_run(args);
    else if (str_eq(cmd, "mkdemo"))cmd_mkdemo();
    else { kputs("Unknown command: "); kputs(cmd); kputs("  (try 'help')\n"); }
}

static void shell_run(void) {
    for (;;) {
        cmdlen = 0;
        shell_prompt();
        for (;;) {
            char c = kb_read();
            if ((uint8_t)c < 0x20 && c != '\n' && c != '\b') continue; /* arrows etc. */
            if (c == '\n') { kput('\n'); break; }
            else if (c == '\b') { if (cmdlen > 0) { cmdlen--; kput('\b'); } }
            else if (cmdlen < CMD_MAX - 1) { cmdline[cmdlen++] = c; kput(c); }
        }
        process_cmd();
    }
}

/* ---- boot screen helpers ---- */
static void vga_grad_bar(void)
{
    if (gfx_active()) { gfx_grad_bar(); return; }
    static const uint8_t pal[8] = {
        C_DGRAY, C_BLUE, C_CYAN, C_LCYAN, C_WHITE, C_LCYAN, C_CYAN, C_BLUE
    };
    for (int x = 0; x < VGA_COLS; x++) {
        uint8_t a = pal[x * 8 / VGA_COLS];
        VGA_MEM[vga_row * VGA_COLS + x] = ((uint16_t)a << 8) | 0xDB; /* full block */
    }
    vga_row++;
    vga_col = 0;
    vga_move_cursor();
}

static void vga_center(const char *s)
{
    int pad = (VGA_COLS - str_len(s)) / 2;
    for (int i = 0; i < pad; i++) vga_putc(' ');
    vga_puts(s);
}

static void boot_tag(int ok, const char *msg, const char *detail)
{
    set_color(C_DGRAY); kputs("  [");
    if (ok) { set_color(C_LGREEN); kputs(" OK "); }
    else    { set_color(C_LRED);   kputs("FAIL"); }
    set_color(C_DGRAY); kputs("] ");
    set_color(C_LCYAN); kputs(msg);
    for (int i = str_len(msg); i < 7; i++) kput(' ');
    reset_color();
    set_color(C_DGRAY); kputs("- "); kputs(detail);
    reset_color();
    kput('\n');
}

static void banner(void) {
    static const char *logo[] = {
        "\xDB\xDB   \xDB  \xDB\xDB\xDB\xDB  \xDB    \xDB  \xDB\xDB\xDB\xDB   \xDB\xDB\xDB\xDB   \xDB\xDB\xDB\xDB ",
        "\xDB \xDB  \xDB \xDB    \xDB \xDB    \xDB \xDB    \xDB \xDB    \xDB \xDB     ",
        "\xDB  \xDB \xDB \xDB    \xDB  \xDB  \xDB  \xDB\xDB\xDB\xDB\xDB\xDB \xDB    \xDB  \xDB\xDB\xDB\xDB ",
        "\xDB   \xDB\xDB \xDB    \xDB  \xDB  \xDB  \xDB    \xDB \xDB    \xDB      \xDB",
        "\xDB   \xDB\xDB  \xDB\xDB\xDB\xDB    \xDB\xDB    \xDB    \xDB  \xDB\xDB\xDB\xDB   \xDB\xDB\xDB\xDB ",
    };
    vga_clear();
    vga_grad_bar();
    vga_putc('\n');
    if (gfx_active()) {
        /* big logo: render the block-art lines at 2x scale, centered */
        int wide = 41 * 16;                       /* longest line at scale 2 */
        int x0 = (gfx_cols() * 8 - wide) / 2;
        if (x0 < 0) x0 = 0;
        for (int i = 0; i < 5; i++) {
            const char *ln = logo[i];
            int x = x0;
            for (int k = 0; ln[k]; k++, x += 16)
                gfx_blit_char(x, 32 + i * 32, ln[k], C_LCYAN, 2);
        }
        /* the 2x logo spans 10 text rows + 1 blank; advance the cursor */
        for (int i = 0; i < 11; i++) vga_putc('\n');
    } else {
        set_color(C_LCYAN);
        for (int i = 0; i < 5; i++) { vga_center(logo[i]); vga_putc('\n'); }
        vga_putc('\n');
    }
    set_color(C_LGRAY);
    vga_center("a tiny 32-bit operating system"); vga_putc('\n');
    set_color(C_DGRAY);
    vga_center(__DATE__); vga_putc('\n');
    vga_putc('\n');
    vga_grad_bar();
    vga_putc('\n');
    reset_color();
    serial_puts("=== NovaOS kernel ===\n");
}

void kmain(void) {
    serial_init();
    int gfx = gfx_init();      /* capture font, find LFB, set VBE mode */
    banner();
    boot_tag(1, "cpu",    "protected mode, 32-bit");
    if (gfx) boot_tag(1, "video", "1024x768x32 LFB (VBE driver)");
    else {
        static const char *why[4] = { "", "font plane unreadable",
                                      "no PCI display BAR", "no VBE extension" };
        boot_tag(1, "video", why[gfx_fail_stage()]);
        if (gfx_fail_stage() == 3) {
            kputs("         id="); kput_hex(gfx_dbg_id());
            kputs(" bar="); kput_hex(gfx_dbg_bar()); kput('\n');
        }
    }
    boot_tag(1, "com1",   "115200 8N1");

    int fr = fs_init();
    if(fr == 0)      boot_tag(1, "novafs", "mounted");
    else if(fr == 1) boot_tag(1, "novafs", "fresh disk, auto-formatted");
    else             boot_tag(0, "novafs", "disk I/O error");

    if (acpi_init() == 0) {
        if (g_acpi.s5_found) boot_tag(1, "acpi", "\\_S5 found, poweroff ready");
        else                 boot_tag(1, "acpi", "tables found");
    } else {
        boot_tag(0, "acpi", "tables not found");
    }
    int mse = mouse_init();
    boot_tag(mse == 0, "input",  mse == 0 ? "keyboard + PS/2 mouse"
                                          : "keyboard (no aux mouse)");
    ring3_init(&nxp_api);
    boot_tag(1, "ring3", "paging + TSS + IDT, user mode ready");
    nxp_api.scr_w = gfx_active() ? (uint32_t)(gfx_cols() * 8) : 0;
    nxp_api.scr_h = gfx_active() ? (uint32_t)(gfx_rows() * 16) : 0;

    kput('\n');
    kputs("Type 'help' for a list of commands.\n");
    shell_run();
}
