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
#include "net.h"
#include "feature.h"
#include "cnfont.h"

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
    } else if (c == '\v') {             /* cursor left WITHOUT erase (line edit) */
        if (vga_col > 0) vga_col--;
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

/* ---- cell-addressed console ops (TUI primitives) ----
 * attr = VGA attribute byte (fg | bg<<4). Works on both backends:
 * the VBE LFB console renders via gfx_cell (per-cell colors, no
 * cursor/scroll side effects), the 80x25 fallback writes VGA_MEM. */
void con_put(int x, int y, char ch, uint8_t attr)
{
    if (x < 0 || y < 0) return;
    if (gfx_active()) { gfx_cell(x, y, ch, attr & 0x0F, (attr >> 4) & 0x0F); return; }
    if (x >= VGA_COLS || y >= VGA_ROWS) return;
    VGA_MEM[y * VGA_COLS + x] = ((uint16_t)attr << 8) | (uint8_t)ch;
}

void con_fill(int x, int y, int w, int h, char ch, uint8_t attr)
{
    for (int r = 0; r < h; r++)
        for (int c = 0; c < w; c++)
            con_put(x + c, y + r, ch, attr);
}

/* hide/show the text-mode hardware cursor. gfx path: the LFB console has
 * no autonomous cursor (it is only drawn by gfx_putc, which a fullscreen
 * TUI never calls, and redraws wipe the underline), so only the VGA
 * fallback needs real cursor control. */
static int cursor_start = -1;
void con_cursor(int on)
{
    if (gfx_active()) return;
    if (on) {
        if (cursor_start >= 0) { outb(0x3D4, 0x0A); outb(0x3D5, (uint8_t)cursor_start); }
    } else {
        if (cursor_start < 0) { outb(0x3D4, 0x0A); cursor_start = inb(0x3D5) & 0x1F; }
        outb(0x3D4, 0x0A); outb(0x3D5, 0x20);       /* bit5 set: cursor off */
    }
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

/* clear the shared console - the kernel calls this when the LAST process
 * exits, so the shell always gets a clean screen (paging.c) */
void kcls(void) { vga_clear(); }

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
    if (sc == 0x01) return 27;              /* ESC (apps like paint quit on it) */
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

/* hotkey seen by kb_poll, consumed by the syscall dispatcher */
static int g_hotkey;            /* 0x57=F11 / 0x58=F12 seen while polling */
int kb_take_hotkey(void) { int h = g_hotkey; g_hotkey = 0; return h; }

/* non-blocking key read for kernel-side service loops (net.c udpecho) */
static int kb_poll(void);
int shell_kb_poll(void) { return kb_poll(); }

/* scheduler hotkeys (paging.c irq0): consume raw make-scancode `raw`
 * from the PS/2 ring if it is pending (F11 = 0x57, F12 = 0x58) */
int kb_take_raw(uint8_t raw)
{
    ps2_drain();
    int i = khead;
    while (i != ktail) {
        if (kbuf[i] == raw) { kbuf[i] = 0; return 1; }
        i = (i + 1) % KBUF;
    }
    return 0;
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
        if (v == 0x57 || v == 0x58) { g_hotkey = v; continue; }  /* scheduler hotkeys */
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

/* binary month/day for the feature subsystem (holiday countdown);
 * consolidates the BCD conversion the date path does */
void rtc_get_md(int *mon, int *day)
{
    uint8_t m = cmos_read(0x08);
    uint8_t d = cmos_read(0x07);
    uint8_t regb = cmos_read(0x0B);
    if (!(regb & 0x04)) { m = bcd_to_bin(m); d = bcd_to_bin(d); }
    if (mon) *mon = m;
    if (day) *day = d;
}

/* append helpers for building a date string into a buffer (shared by the
 * kernel 'date' command and the SYS_GETDATE syscall) */
static int buf_ch(char *buf, int n, int max, char c)
{
    if (n < max - 1) buf[n++] = c;
    return n;
}
static int buf_str(char *buf, int n, int max, const char *s)
{
    while (*s && n < max - 1) buf[n++] = *s++;
    return n;
}
static int buf_dec(char *buf, int n, int max, unsigned v)
{
    char tmp[12]; int k = 0;
    if (v == 0) tmp[k++] = '0';
    while (v) { tmp[k++] = (char)('0' + (v % 10)); v /= 10; }
    while (k-- > 0 && n < max - 1) buf[n++] = tmp[k];
    return n;
}

/* Format current RTC date/time as "Date: Day MM/DD/YYYY  Time: HH:MM:SS"
 * (no trailing newline) into buf. Returns byte count, or 0 if too small. */
static int format_date(char *buf, int max)
{
    if (max < 48) return 0;
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

    uint16_t year = (y_raw < 80) ? (uint16_t)(2000 + y_raw)
                                 : (uint16_t)(1900 + y_raw);

    static const char *wday[] = {
        "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"
    };
    const char *dn = (dow >= 1 && dow <= 7) ? wday[dow - 1] : "???";

    int n = 0;
    n = buf_str(buf, n, max, "Date: ");
    n = buf_str(buf, n, max, dn);  n = buf_ch(buf, n, max, ' ');
    n = buf_dec(buf, n, max, mon); n = buf_ch(buf, n, max, '/');
    n = buf_dec(buf, n, max, day); n = buf_ch(buf, n, max, '/');
    n = buf_dec(buf, n, max, year);
    n = buf_str(buf, n, max, "  Time: ");
    if (hour < 10) n = buf_ch(buf, n, max, '0');
    n = buf_dec(buf, n, max, hour); n = buf_ch(buf, n, max, ':');
    if (min  < 10) n = buf_ch(buf, n, max, '0');
    n = buf_dec(buf, n, max, min);  n = buf_ch(buf, n, max, ':');
    if (sec  < 10) n = buf_ch(buf, n, max, '0');
    n = buf_dec(buf, n, max, sec);
    buf[n] = 0;
    return n;
}

static void cmd_date(void)
{
    char buf[64];
    if (format_date(buf, sizeof buf) > 0) { kputs(buf); kput('\n'); }
}

/* ---- Shell ---- */
#define CMD_MAX 128
#define TXT_MAX 16384   /* kernel cat/write buffer. Files on disk may be
                         * 68608 B (Ring3 readfile/writefile see the full
                         * size) - keep kernel BSS well below the fixed
                         * page tables at 0x141000 */
static char cmdline[CMD_MAX];
static int  cmdlen = 0;
static char textbuf[TXT_MAX];
static char cwdbuf[128];

/* login (defined further below) */
static int g_logout = 0;
static char g_cur_user[FS_NAME_LEN + 1] = "root";
static int is_root(void);
static void login_run(void);
static void cmd_passwd(void);
static void cmd_logout(void);
static void cmd_useradd(const char *name);
static void cmd_userdel(const char *name);
static void cmd_su(const char *name);

/* ---- global settings: nova.cfg in the NovaFS root ----
 * The kernel reads it once right after the filesystem mounts (accent
 * recolors the shell/login, quiet suppresses banner + self-test) and
 * again on sysop 18 whenever the TUI settings panel saves the file. */
uint8_t g_accent = C_LCYAN;
int      g_quiet = 0;

static void cfg_apply_text(char *buf)
{
    char *p = buf;
    while (*p) {
        char *line = p;
        while (*p && *p != '\n') p++;
        if (*p) *p++ = 0;
        char *eq = line;
        while (*eq && *eq != '=') eq++;
        if (!*eq) continue;
        *eq = 0;
        char *v = eq + 1;
        if (str_eq(line, "accent")) {
            static const char *names[5] = { "cyan","green","yellow","magenta","white" };
            static const uint8_t vals[5] = { C_LCYAN, C_LGREEN, C_YELLOW, C_LMAGENTA, C_WHITE };
            for (int i = 0; i < 5; i++)
                if (str_eq(v, names[i])) g_accent = vals[i];
        } else if (str_eq(line, "quiet")) {
            g_quiet = str_eq(v, "on");
        }
    }
}

static void cfg_load(void)
{
    static char cfg_buf[512];
    if (!fs_is_ready()) return;
    int sz = fs_size("nova.cfg");
    if (sz <= 0) return;
    for (int i = 0; i < 512; i++) cfg_buf[i] = 0;
    fs_read("nova.cfg", (uint8_t *)cfg_buf, 511);
    cfg_apply_text(cfg_buf);
}

/* ---- feature command (list / enable / disable / demo hooks) ---- */
static void cmd_feature(const char *args)
{
    char name[FEAT_NAME_MAX];
    name[0] = 0;
    const char *s = args ? args : "";
    while (*s == ' ') s++;
    int k = 0;
    const char *w1 = s;
    while (*s && *s != ' ' && k < FEAT_NAME_MAX - 1) name[k++] = *s++;
    name[k] = 0;
    while (*s == ' ') s++;
    const char *w2 = s;                     /* feature name for enable/disable */

    if (!name[0]) {                         /* list all */
        char lb[512];
        int n = feature_list(lb, (uint32_t)sizeof lb);
        if (!n) { kputs("no features registered\n"); return; }
        kputs("feature              state\n");
        const char *p = lb;
        while (*p) {
            set_color(g_accent);
            while (*p && *p != ' ') kput(*p++);     /* name (padded to 21) */
            reset_color();
            while (*p && *p != '\n') kput(*p++);    /* pad + state */
            kput('\n');                             /* the buffer's newline is
                                                       consumed, not printed */
            if (*p) p++;
        }
        return;
    }
    if (str_eq(name, "enable") || str_eq(name, "disable")) {
        if (!w2 || !*w2) {
            kputs("usage: feature enable NAME | feature disable NAME\n");
            return;
        }
        int on = str_eq(name, "enable");
        int r = feature_set(w2, on);
        if (r < 0) { kputs("feature: no such module\n"); return; }
        kputs(w2); kputs(on ? " enabled (saved to /etc/features.conf)\n"
                            : " disabled (saved to /etc/features.conf)\n");
        return;
    }
    if (str_eq(name, "check")) {            /* demo of the mandatory gate */
        if (!w2 || !*w2) { kputs("usage: feature check NAME\n"); return; }
        kputs(feature_is_enabled(w2) ? "enabled\n" : "disabled\n");
        return;
    }
    if (str_eq(name, "demo")) {             /* call the gated module hooks */
        int h = feat_holiday_days_left();
        kputs("holiday_module.days_left() -> ");
        if (h == -1) kputs("BLOCKED (module disabled)\n");
        else if (h == 0) {
            kputs("TODAY! (national day holiday)\n");
            feat_holiday_banner();
            kputs("  banner drawn on screen\n");
        }
        else {
            kput_dec((unsigned)h); kputs(" day(s) to Oct 1\n");
            feat_holiday_banner();          /* no-op unless it IS the holiday */
        }
        const char *g = feat_cn_greeting();
        kputs("cn_lang_support.greeting() -> ");
        if (!g) kputs("BLOCKED (module disabled)\n");
        else { kputs(g); kput('\n'); }
        return;
    }
    kputs("usage: feature [enable NAME | disable NAME | check NAME | demo]\n");
}

static void shell_prompt(void)
{
    con_cursor(1);      /* a killed TUI may have hidden the hw cursor */
    fs_getcwd(cwdbuf, sizeof(cwdbuf));
    set_color(g_accent); vga_puts("\n");
    set_color(C_LGREEN); vga_puts(g_cur_user);
    set_color(g_accent); vga_puts("@novaos:");
    set_color(C_YELLOW); vga_puts(cwdbuf);
    set_color(C_DGRAY);  vga_puts("#");
    reset_color(); vga_putc(' ');
    serial_puts("\n"); serial_puts(g_cur_user);
    serial_puts("@novaos:"); serial_puts(cwdbuf); serial_puts("# ");
}

static void cmd_help(void)
{
    set_color(C_DGRAY); vga_puts(" --- commands ---\n"); reset_color();
    set_color(g_accent); vga_puts("  help    "); reset_color(); kputs("show this help\n");
    set_color(g_accent); vga_puts("  ver     "); reset_color(); kputs("kernel version\n");
    set_color(g_accent); vga_puts("  about   "); reset_color(); kputs("about NovaOS\n");
    set_color(g_accent); vga_puts("  echo X  "); reset_color(); kputs("print text\n");
    set_color(g_accent); vga_puts("  cls     "); reset_color(); kputs("clear screen\n");
    set_color(g_accent); vga_puts("  date    "); reset_color(); kputs("date and time\n");
    set_color(g_accent); vga_puts("  mem     "); reset_color(); kputs("memory layout\n");
    set_color(g_accent); vga_puts("  acpi    "); reset_color(); kputs("ACPI tables + \\_S5 info\n");
    set_color(g_accent); vga_puts("  netinfo "); reset_color(); kputs("NIC status (auto-dhcp)\n");
    set_color(g_accent); vga_puts("  dhcp    "); reset_color(); kputs("(re)request an IP via DHCP\n");
    set_color(g_accent); vga_puts("  ping IP "); reset_color(); kputs("send 4 ICMP echoes\n");
    set_color(g_accent); vga_puts("  dns NAME"); reset_color(); kputs("resolve a hostname\n");
    set_color(g_accent); vga_puts("  wget H "); reset_color(); kputs("HTTP GET -> NovaFS file\n");
    set_color(g_accent); vga_puts("  udpecho "); reset_color(); kputs("UDP echo server :7777 (q stops)\n");
    set_color(g_accent); vga_puts("  ls      "); reset_color(); kputs("list current directory\n");
    set_color(g_accent); vga_puts("  cd D    "); reset_color(); kputs("change directory (.. / /)\n");
    set_color(g_accent); vga_puts("  mkdir D "); reset_color(); kputs("create directory\n");
    set_color(g_accent); vga_puts("  rmdir D "); reset_color(); kputs("remove directory (must be empty)\n");
    set_color(g_accent); vga_puts("  rd D    "); reset_color(); kputs("force delete dir + contents\n");
    set_color(g_accent); vga_puts("  cat F   "); reset_color(); kputs("show file\n");
    set_color(g_accent); vga_puts("  write F "); reset_color(); kputs("create file (end with .)\n");
    set_color(g_accent); vga_puts("  rm F    "); reset_color(); kputs("delete file\n");
    set_color(g_accent); vga_puts("  chmod F "); reset_color(); kputs("set permissions (e.g. 64 = rw-r--)\n");
    set_color(g_accent); vga_puts("  feature "); reset_color(); kputs("module switches (enable/disable, /etc/features.conf)\n");
    set_color(g_accent); vga_puts("  run F   "); reset_color(); kputs("run .nsh script / start .nxp process\n");
    set_color(g_accent); vga_puts("  procs   "); reset_color(); kputs("list running processes\n");
    set_color(g_accent); vga_puts("  fg      "); reset_color(); kputs("resume suspended processes\n");
    set_color(g_accent); vga_puts("  kill N  "); reset_color(); kputs("end process N (no N = all)\n");
    set_color(g_accent); vga_puts("  mkdemo  "); reset_color(); kputs("create demo.nxp sample program\n");
    set_color(g_accent); vga_puts("  format  "); reset_color(); kputs("format NovaFS\n");
    set_color(g_accent); vga_puts("  fsinfo  "); reset_color(); kputs("filesystem info\n");
    set_color(g_accent); vga_puts("  passwd  "); reset_color(); kputs("change login password\n");
    set_color(g_accent); vga_puts("  useradd "); reset_color(); kputs("create a new user\n");
    set_color(g_accent); vga_puts("  userdel "); reset_color(); kputs("delete a user (root)\n");
    set_color(g_accent); vga_puts("  su U    "); reset_color(); kputs("switch to another user\n");
    set_color(g_accent); vga_puts("  whoami  "); reset_color(); kputs("show current user\n");
    set_color(g_accent); vga_puts("  logout  "); reset_color(); kputs("return to login screen\n");
    set_color(g_accent); vga_puts("  reboot  "); reset_color(); kputs("restart\n");
    set_color(g_accent); vga_puts("  shutdown"); reset_color(); kputs("  power off\n");
    set_color(g_accent); vga_puts("  halt    "); reset_color(); kputs("halt cpu\n");
}

static void cmd_ver(void)
{
    set_color(g_accent); vga_puts("novaos"); reset_color();
    kputs(" v0.3  (32bit)  ");
    set_color(C_DGRAY); vga_puts(__DATE__); reset_color(); kput('\n');
}

static void cmd_about(void)
{
    set_color(g_accent); vga_puts("novaos"); reset_color();
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

    /* only trust ACPI poweroff when there are actual PM1 control ports;
     * otherwise fall through to the PIIX4 / QEMU debug-exit fallbacks */
    if (g_acpi.fadt && (g_acpi.pm1a_cnt || g_acpi.pm1b_cnt)) {
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
    kputs("[shutdown] ACPI poweroff unavailable, trying PIIX4 default (0x604)...\n");
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
 * .nxp program loader / process spawner
 *   format : flat binary, "NXP\x01" magic (4B) + code
 *   slots  : 4 processes, per-slot link base (see paging.c):
 *            name.nxp    -> 0x300000    name.1.nxp -> 0x320000
 *            name.2.nxp  -> 0x340000    name.3.nxp -> 0x360000
 *   entry  : EAX = pointer to nxp_api_t, fresh per-slot stack,
 *            ends via `ret`/api->exit() into the scheduler
 * ============================================================ */

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
    void (*cls)(void);                              /* clear screen */
    void (*set_color)(uint32_t fg);                 /* VGA attr foreground */
    int  (*getuser)(char *buf, uint32_t max);      /* current login name */
    int  (*getdate)(char *buf, uint32_t max);      /* formatted RTC date */
    int  (*readfile)(const char *name, uint8_t *buf, uint32_t max);
                                                   /* read a NovaFS file */
    int  (*spawn)(const char *name);              /* new process        */
    int  (*procs)(char *buf, uint32_t max);       /* list live processes */
    int  (*writefile)(const char *name, const uint8_t *data, uint32_t len);
                                                  /* save a file (owned) */
    int  (*listdir)(char *buf, uint32_t max);     /* ls into a buffer   */
    int  (*fsop)(uint32_t op, const char *name);  /* fs ops by opcode   */
    int  (*sysop)(uint32_t op, const char *name); /* system ops by opcode */
    uint32_t (*ticks)(void);                      /* 10 ms since boot   */
    void (*putcell)(uint32_t x, uint32_t y, uint32_t ch, uint32_t attr);
                                                  /* TUI: write one cell */
    void (*cellfill)(uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                     uint32_t chattr);            /* chattr = ch<<8|attr */
    void (*cputs)(uint32_t x, uint32_t y, const char *s, uint32_t attr);
                                                  /* TUI: text at cells  */
    void (*cursor)(uint32_t on);                  /* TUI: hide/show cursor */
} nxp_api_t;

static void nxp_api_putc(char c)              { kput(c); }
static void nxp_api_puts(const char *s)       { if (s) kputs(s); }
static int  nxp_api_getchar(void)             { return kb_poll(); }   /* -1 = empty */
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
    if (!s) return;
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
static void nxp_api_cls(void)                  { vga_clear(); }
static void nxp_api_setcolor(uint32_t fg)       { set_color((uint8_t)fg); }
static int  nxp_api_getuser(char *buf, uint32_t max)
{
    if (!buf || max == 0) return 0;
    uint32_t i = 0;
    while (i + 1 < max && g_cur_user[i]) { buf[i] = g_cur_user[i]; i++; }
    buf[i] = 0;
    return (int)i;
}
static int nxp_api_getdate(char *buf, uint32_t max) { return format_date(buf, (int)max); }

static int slot_from_name(const char *name);
static int nxp_load_slot(const char *name, int slot);

/* spawn an .nxp as a new process (slot chosen by the file name) */
static int nxp_api_spawn(const char *name)
{
    if (!name || !*name) return -1;
    int slot = slot_from_name(name);
    if (nxp_load_slot(name, slot) < 0) return -1;
    return proc_spawn(slot, name);
}

/* list live processes as "pid name" lines (nsh `procs` command) */
static int nxp_api_procs(char *buf, uint32_t max)
{
    if (!buf || max == 0) return -1;
    return proc_list(buf, max);
}

/* write a file from Ring3 (NovaFS ownership checks apply) */
static int nxp_api_writefile(const char *name, const uint8_t *data, uint32_t len)
{
    if (!name || !data || !len) return -1;
    return fs_write(name, data, len);
}

/* 10 ms since boot - the clock games and animation are built on */
static uint32_t nxp_api_ticks(void) { return proc_ticks(); }

/* ---- TUI primitives (cell-addressed console, see con_put above) ---- */
static void nxp_api_putcell(uint32_t x, uint32_t y, uint32_t ch, uint32_t attr)
{
    con_put((int)x, (int)y, (char)ch, (uint8_t)attr);
}
static void nxp_api_cellfill(uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                             uint32_t chattr)     /* chattr = ch<<8 | attr */
{
    if (w == 0 || h == 0) return;
    if (w > 256) w = 256;
    if (h > 128) h = 128;
    con_fill((int)x, (int)y, (int)w, (int)h, (char)(chattr >> 8), (uint8_t)(chattr & 0xFF));
}
static void nxp_api_cputs(uint32_t x, uint32_t y, const char *s, uint32_t attr)
{
    if (!s) return;
    int cx = (int)x, n = 0;
    while (*s && n++ < 256) con_put(cx++, (int)y, *s++, (uint8_t)attr);
}
static void nxp_api_cursor(uint32_t on)
{
    con_cursor((int)on);
}

/* ---- the syscall layer reuses the shell command implementations ---- */
static void cmd_mkdir(const char *name);
static void cmd_rmdir(const char *name);
static void cmd_rd(const char *name);
static void cmd_rm(const char *name);
static void cmd_chmod(const char *args);
static void cmd_cd(const char *name);
static void cmd_format(void);
static void cmd_fsinfo(void);
static void cmd_useradd(const char *name);
static void cmd_userdel(const char *name);
static void cmd_passwd(void);
static void cmd_mkdemo(void);
static void cmd_acpi(void);
static void cmd_reboot(void);
static void cmd_shutdown(void);
static void cmd_halt(void);

/* ls into a caller buffer, formatted like the shell's ls */
static int   g_ls_n;
static char *g_ls_p;
static uint32_t g_ls_max, g_ls_o;
static void ls_putc(char c) { if (g_ls_o + 1 < g_ls_max) g_ls_p[g_ls_o++] = c; }
static void ls_puts(const char *s) { while (*s) ls_putc(*s++); }
static void ls_dec(uint32_t v)
{
    char t[12]; int k = 0;
    if (!v) t[k++] = '0';
    while (v) { t[k++] = (char)('0' + v % 10); v /= 10; }
    while (k--) ls_putc(t[k]);
}
/* render the mode byte as "rwxrwx" (owner nibble + others nibble) */
static void mode_str(uint8_t mode, char *out)
{
    int p = 0;
    for(int half = 0; half < 2; half++){
        uint8_t n = half ? (uint8_t)(mode & 0xF) : (uint8_t)(mode >> 4);
        out[p++] = (n & 4) ? 'r' : '-';
        out[p++] = (n & 2) ? 'w' : '-';
        out[p++] = (n & 1) ? 'x' : '-';
    }
    out[6] = 0;
}

static void ls_collect(const char *name, int type, uint32_t size, uint8_t mode)
{
    char mb[7];
    mode_str(mode, mb);
    g_ls_n++;
    if (!g_ls_p) return;
    ls_puts(type == T_DIR ? "  [DIR]  " : "  [FILE] ");
    ls_puts(mb); ls_putc(' ');
    ls_puts(name);
    if (type != T_FILE) { ls_putc('\n'); return; }
    ls_puts("   ("); ls_dec(size); ls_puts(" B)\n");
}
static int nxp_api_listdir(char *buf, uint32_t max)
{
    if (!buf || max == 0) return -1;
    g_ls_p = buf; g_ls_max = max; g_ls_o = 0; g_ls_n = 0;
    fs_list(ls_collect);
    if (g_ls_o < max) buf[g_ls_o] = 0;
    return g_ls_n;
}

/* filesystem ops by opcode: 1=mkdir 2=rmdir 3=rd 4=rm 5=cd 6=format 7=fsinfo */
static int nxp_api_fsop(uint32_t op, const char *name)
{
    switch (op) {
    case 1: cmd_mkdir(name);  return 0;
    case 2: cmd_rmdir(name);  return 0;
    case 3: cmd_rd(name);     return 0;
    case 4: cmd_rm(name);     return 0;
    case 5: cmd_cd(name);     return 0;
    case 6: cmd_format();     return 0;
    case 7: cmd_fsinfo();     return 0;
    default: return -1;
    }
}

/* system ops by opcode: 1=useradd 2=userdel 3=passwd 4=su 5=mkdemo
 * 6=acpi 7=reboot 8=shutdown 9=halt 10=fg(resume all) 11=kill pid
 * 12=netinfo 13=ping 14=dhcp 15=dns 16=udpecho 17=wget
 * 18=cfg reload 19=feature list 20=feature set (name "N=enable|disable") */
static int nxp_api_sysop(uint32_t op, const char *name)
{
    switch (op) {
    case 1:  cmd_useradd(name); return 0;
    case 2:  cmd_userdel(name); return 0;
    case 3:  cmd_passwd();      return 0;
    case 4:  cmd_su(name);      return 0;
    case 5:  cmd_mkdemo();      return 0;
    case 6:  cmd_acpi();        return 0;
    case 7:  cmd_reboot();      return 0;
    case 8:  cmd_shutdown();    return 0;
    case 9:  cmd_halt();        return 0;
    case 10: proc_resume_all(); return 0;
    case 11:
        if (name && name[0] >= '1' && name[0] <= '4' && name[1] == 0) {
            int r = proc_kill(name[0] - '0');
            if (r == -2) kputs("kill: permission denied (not your process)\n");
            return r;
        }
        return -1;
    case 12: cmd_netinfo();     return 0;
    case 13: cmd_net_ping(name); return 0;
    case 14: cmd_net_dhcp();    return 0;
    case 15: cmd_net_dns(name); return 0;
    case 16: cmd_net_udpecho(); return 0;
    case 17: cmd_wget(name);    return 0;
    case 18: cfg_load();        return 0;
    case 19: return feature_list((char *)name, 512);
    case 20: {
        /* name = "module=enable" or "module=disable" */
        if (!name) return -1;
        char mod[FEAT_NAME_MAX];
        int k = 0;
        const char *eq = name;
        while (*eq && *eq != '=' && k < FEAT_NAME_MAX - 1) mod[k++] = *eq++;
        mod[k] = 0;
        if (*eq != '=') return -1;
        return feature_set(mod, str_eq(eq + 1, "enable"));
    }
    default: return -1;
    }
}

/* read a NovaFS file (current dir) into a user buffer - the file access
 * nsh.nxp needs to load .nsh scripts, handy for any other program too */
static int nxp_api_readfile(const char *name, uint8_t *buf, uint32_t max)
{
    if (!name || !buf || max == 0 || !fs_is_ready()) return -1;
    int sz = fs_size(name);
    if (sz < 0) return -1;
    if (fs_read(name, buf, max) == -2) return -1;   /* read permission denied */
    if (sz > (int)max) sz = (int)max;
    return sz;
}

static nxp_api_t nxp_api = {
    0x3150584Eu, 1,
    nxp_api_putc, nxp_api_puts, nxp_api_getchar, nxp_api_exit,
    0, 0,
    nxp_api_pixel, nxp_api_fill, nxp_api_text,
    nxp_api_getkey,
    nxp_api_mouse,
    nxp_api_getpixel,
    nxp_api_cls, nxp_api_setcolor, nxp_api_getuser, nxp_api_getdate,
    nxp_api_readfile, nxp_api_spawn, nxp_api_procs, nxp_api_writefile,
    nxp_api_listdir, nxp_api_fsop, nxp_api_sysop, nxp_api_ticks,
    nxp_api_putcell, nxp_api_cellfill, nxp_api_cputs, nxp_api_cursor
};

/* "name.D.nxp" selects process slot D (0-3); plain "name.nxp" = slot 0 */
static int slot_from_name(const char *name)
{
    int n = str_len(name);
    if (n > 6 &&
        name[n-1]=='p' && name[n-2]=='x' && name[n-3]=='n' &&
        name[n-4]=='.' && name[n-6]=='.' &&
        name[n-5] >= '0' && name[n-5] <= '3')
        return name[n-5] - '0';
    return 0;
}

/* load an .nxp image into a process slot; 0 = ok, -1 = error (printed).
 * Files up to ~67 KB (6 direct + 128 indirect blocks) are read straight
 * into the slot; the rest of the 128 KB slot is zeroed as slack + BSS. */
static int nxp_load_slot(const char *name, int slot)
{
    if(!fs_is_ready()){ kputs("NovaFS not formatted. Use 'format'.\n"); return -1; }
    int sz = fs_size(name);
    if(sz < 5){ kputs("run: no such file\n"); return -1; }
    if(!fs_may_exec(name)){ kputs("run: permission denied\n"); return -1; }
    uint8_t *dst = (uint8_t *)proc_slot_base(slot);
    fs_read(name, dst, 0x1F000u);
    if(dst[0]!='N' || dst[1]!='X' || dst[2]!='P' || dst[3]!=1){
        kputs("run: not a .nxp program (bad magic - .nsh? try nsh.nxp)\n"); return -1;
    }
    if(sz > 0x1F000) sz = 0x1F000;
    for(int i = sz; i < 0x1F000; i++) dst[i] = 0;   /* slack + big BSS */
    return 0;
}

/* set by cmd_run/cmd_fg around main_checkpoint: distinguishes the first
 * dive into a process (resume lands after the park and must continue into
 * jmp_user) from ring3_leave's resume at the same spot (print the outcome
 * and fall back to the shell prompt) */
static int g_park_first = 0;

/* ================= .nsh script engine (kernel side) =================
 * Same batch-compatible subset as nsh.nxp's engine (keep the syntax in
 * sync with programs/nsh.c): rem/::/@/echo on|off/set %var%/if [not]
 * A==B/goto labels/pause/exit. Everything else is delegated to
 * process_cmd(), so a script can use EVERY NovaSh command (ls, mkdir,
 * write, useradd, run ...). */
#define NSH_MAX 3072
#define NSH_L   128
#define NSVARS  8
#define NNAME   12
#define NVAL    32

static char nsh_sbuf[NSH_MAX];
static char nsh_line[NSH_L];
static char nsh_exp[NSH_L + NVAL];
static char nsv_name[NSVARS][NNAME];
static char nsv_val[NSVARS][NVAL];
static int  nsh_echo_on = 1;
static int  nsh_in_script = 0;
static int  nsh_goto_flag = 0;
static char nsh_goto_target[24];

static void process_cmd(void);           /* delegation target (below) */

static char *nsh_var_get(const char *name, int len)
{
    for (int i = 0; i < NSVARS; i++) {
        if (!nsv_name[i][0] || str_len(nsv_name[i]) != len) continue;
        int j = 0;
        while (j < len && nsv_name[i][j] == name[j]) j++;
        if (j == len) return nsv_val[i];
    }
    return 0;
}

static void nsh_expand(const char *in, char *out)
{
    int o = 0;
    for (int i = 0; in[i] && o < NSH_L + NVAL - 2; i++) {
        if (in[i] != '%') { out[o++] = in[i]; continue; }
        int j = i + 1;
        while (in[j] && in[j] != '%') j++;
        if (!in[j]) { out[o++] = '%'; continue; }        /* unmatched % */
        char *v = nsh_var_get(in + i + 1, j - i - 1);
        if (!v) { out[o++] = '%'; continue; }            /* unknown var */
        while (*v && o < NSH_L + NVAL - 2) out[o++] = *v++;
        i = j;
    }
    out[o] = 0;
}

/* one script line; returns 1 = stop the script */
static int nsh_exec(char *line, int silent)
{
    if (*line == '@') { silent = 1; line++; }
    while (*line == ' ') line++;
    if (!*line || *line == ':') return 0;                /* empty / label */
    if (!silent) { kputs(line); kput('\n'); }

    int i = 0;
    while (line[i] && line[i] != ' ') i++;
    char *args = line + i;
    if (*args) { *args = 0; args++; while (*args == ' ') args++; }
    char *cmd = line;

    if (str_eq(cmd, "rem")) return 0;
    if (str_eq(cmd, "echo")) {
        if (!*args) { kput('\n'); return 0; }
        if (str_eq(args, "off")) { nsh_echo_on = 0; return 0; }
        if (str_eq(args, "on"))  { nsh_echo_on = 1; return 0; }
        kputs(args); kput('\n');
        return 0;
    }
    if (str_eq(cmd, "pause")) { kputs("Press a key\n"); kb_read(); return 0; }
    if (str_eq(cmd, "set")) {
        char *eq = 0;
        for (char *q = args; *q; q++) if (*q == '=') { eq = q; break; }
        if (!eq) return 0;
        *eq = 0;
        char *nm = args, *vv = eq + 1;
        while (*vv == ' ') vv++;
        int nl = str_len(nm);
        while (nl && nm[nl - 1] == ' ') nm[--nl] = 0;
        int vl = str_len(vv);
        if (!nl || nl >= NNAME || vl >= NVAL) { kputs("set: bad\n"); return 0; }
        int slot = -1;
        for (int k = 0; k < NSVARS && slot < 0; k++)
            if (nsv_name[k][0] && str_eq(nsv_name[k], nm)) slot = k;
        if (slot < 0)
            for (int k = 0; k < NSVARS && slot < 0; k++)
                if (!nsv_name[k][0]) slot = k;
        if (slot < 0) { kputs("set: full\n"); return 0; }
        for (int k = 0; k < nl; k++) nsv_name[slot][k] = nm[k];
        nsv_name[slot][nl] = 0;
        for (int k = 0; k < vl; k++) nsv_val[slot][k] = vv[k];
        nsv_val[slot][vl] = 0;
        return 0;
    }
    if (str_eq(cmd, "goto")) {
        if (*args == ':') args++;
        int m = 0;
        while (args[m] && args[m] != ' ' && m < 23) {
            nsh_goto_target[m] = args[m];
            m++;
        }
        nsh_goto_target[m] = 0;
        if (m) nsh_goto_flag = 1;
        return 0;
    }
    if (str_eq(cmd, "if")) {
        char *s = args;
        int invert = 0;
        if (s[0] == 'n' && s[1] == 'o' && s[2] == 't' && (s[3] == ' ' || s[3] == 0)) {
            invert = 1;
            s += 3;
            while (*s == ' ') s++;
        }
        char *eq = 0;
        for (char *q = s; q[0] && q[1]; q++)
            if (q[0] == '=' && q[1] == '=') { eq = q; break; }
        if (!eq) { kputs("need ==\n"); return 0; }
        *eq = 0;
        char *L = s, *R = eq + 2;
        while (*R == ' ') R++;
        char *rest = R;
        while (*rest && *rest != ' ') rest++;
        if (*rest) { *rest = 0; rest++; while (*rest == ' ') rest++; }
        int c2 = str_eq(L, R);
        if (invert) c2 = !c2;
        if (c2 && *rest) return nsh_exec(rest, 1);
        return 0;
    }
    if (str_eq(cmd, "exit")) return 1;                   /* stop the script */

    /* delegate: a real NovaSh command, rebuilt into the shell buffer */
    int n = 0;
    for (char *q = cmd; *q && n < CMD_MAX - 1; q++) cmdline[n++] = *q;
    if (*args && n < CMD_MAX - 1) cmdline[n++] = ' ';
    for (char *q = args; *q && n < CMD_MAX - 1; q++) cmdline[n++] = *q;
    cmdline[n] = 0;
    cmdlen = n;
    process_cmd();
    return 0;
}

static void nsh_run_script(const char *file)
{
    if (nsh_in_script) { kputs("run: no nested scripts\n"); return; }
    if(!fs_is_ready()){ kputs("NovaFS not formatted.\n"); return; }
    int n = fs_size(file);
    if(n <= 0){ kputs("run: no such file\n"); return; }
    if(!fs_may_read(file)){ kputs("run: permission denied\n"); return; }
    for(int i = 0; i < NSH_MAX; i++) nsh_sbuf[i] = 0;
    fs_read(file, (uint8_t*)nsh_sbuf, NSH_MAX - 1);
    nsh_in_script = 1;
    char *lp = nsh_sbuf;
    while (*lp) {
        int rl = 0;
        char *e = lp;
        while (*e && *e != '\n') {
            if (*e != '\r' && rl < NSH_L - 1) nsh_line[rl++] = *e;
            e++;
        }
        nsh_line[rl] = 0;
        nsh_expand(nsh_line, nsh_exp);
        int stop = nsh_exec(nsh_exp, !nsh_echo_on);
        if (nsh_goto_flag) {
            char *q = nsh_sbuf;
            int found = 0;
            while (*q && !found) {
                char *e2 = q;
                while (*e2 && *e2 != '\n') e2++;
                char sv = *e2;
                *e2 = 0;
                char *t2 = q;
                while (*t2 == ' ') t2++;
                if (*t2 == ':') {
                    t2++;
                    int m = 0;
                    while (nsh_goto_target[m] && t2[m] == nsh_goto_target[m]) m++;
                    if (!nsh_goto_target[m] && (t2[m] == 0 || t2[m] == ' ')) found = 1;
                }
                *e2 = sv;
                if (!found) q = sv ? e2 + 1 : e2;
            }
            if (found) { lp = q; nsh_goto_flag = 0; continue; }
            kputs("no label\n");
            break;
        }
        if (stop) break;
        lp = *e ? e + 1 : e;
    }
    nsh_in_script = 0;
    nsh_goto_flag = 0;
}

/* shell `run`: a .nsh file runs right here in the kernel; a .nxp
 * spawns, parks the shell, and lets the scheduler run until every
 * process is gone (or F12 kills them all) */
static void cmd_run(const char *name)
{
    if(!name || !*name){ kputs("usage: run <file.nxp | file.nsh>\n"); return; }
    int nl = str_len(name);
    if (nl > 4 && str_eq(name + nl - 4, ".nsh")) { nsh_run_script(name); return; }
    int slot = slot_from_name(name);
    if(nxp_load_slot(name, slot) < 0) return;
    int pid = proc_spawn(slot, name);
    if(pid < 0){ kputs("run: slot busy (try name.1/2/3.nxp)\n"); return; }
    kputs("[proc] started "); kputs(name);
    kputs(" pid="); kput_dec((unsigned)pid);
    kputs("  (F11 suspend / F12 kill)\n");
    g_park_first = 1;                      /* the resume below is the first entry */
    main_checkpoint();                     /* park the shell context */
    if (!g_park_first) {                   /* scheduler released us */
        kputs(proc_any() ? "[proc] suspended - 'fg' resumes, 'kill' ends\n"
                         : "[proc] all processes exited\n");
        return;
    }
    g_park_first = 0;
    proc_set_current(slot);                /* first tick must save, not roll back */
    jmp_user(proc_frame(slot));            /* in until all exit (ring3_leave resumes) */
}

/* shell `procs`: with F11 available, suspended processes are visible here */
static void cmd_procs(void)
{
    char pb[128];
    int n = proc_list(pb, sizeof pb);
    if (n <= 0) { kputs("no processes (run X.nxp to spawn)\n"); return; }
    kputs("pid st name\n");
    kputs(pb);
}

/* shell `fg`: thaw suspended processes and re-enter the scheduler */
static void cmd_fg(void)
{
    if (!proc_stopped_any()) { kputs("fg: nothing suspended\n"); return; }
    proc_resume_all();
    int slot = proc_next();
    if (slot < 0) { kputs("fg: nothing to run\n"); return; }
    kputs("[proc] resumed\n");
    g_park_first = 1;
    main_checkpoint();
    if (!g_park_first) {                   /* scheduler released us */
        kputs(proc_any() ? "[proc] suspended - 'fg' resumes, 'kill' ends\n"
                         : "[proc] all processes exited\n");
        return;
    }
    g_park_first = 0;
    proc_set_current(slot);
    jmp_user(proc_frame(slot));            /* in until all exit (ring3_leave resumes) */
}

/* shell `kill [pid]`: end one process, or all of them */
static void cmd_kill(const char *args)
{
    if (!proc_any()) { kputs("kill: no processes\n"); return; }
    if (!args || !*args) { proc_kill_all(); kputs("killed all\n"); return; }
    if (args[0] >= '1' && args[0] <= '4' && args[1] == 0) {
        if (proc_kill(args[0] - '0') < 0) kputs("kill: no such pid\n");
        else kputs("killed\n");
        return;
    }
    kputs("usage: kill [pid 1-4]\n");
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



static void ls_cb(const char *name, int type, uint32_t size, uint8_t mode)
{
    char mb[7];
    mode_str(mode, mb);
    g_file_count++;
    if(type == T_DIR){
        set_color(C_LBLUE);
        kputs("  [DIR]  ");
    }else{
        set_color(C_LGRAY);
        kputs("  [FILE] ");
    }
    reset_color();
    set_color(C_DGRAY); kputs(mb); kput(' '); reset_color();
    kputs(name);
    if(type == T_FILE){ kputs("   ("); kput_dec(size); kputs(" B)"); }
    kput('\n');
}

static void count_cb(const char *name, int type, uint32_t size, uint8_t mode)
{
    (void)name; (void)type; (void)size; (void)mode;
    g_file_count++;
}

static void cmd_fsinfo(void)
{
    if(!fs_is_ready()){ kputs("NovaFS not formatted. Use 'format'.\n"); return; }
    uint32_t ub = 0, ui = 0, uby = 0;
    fs_space(&ub, &ui, &uby);
    kputs("NovaFS disk:\n");
    kputs("  blocks used : "); kput_dec(ub); kputs(" / 32768 (");
    kput_dec(32768u - ub); kputs(" free, 512B each)\n");
    kputs("  space used  : "); kput_dec(uby); kputs(" B / 16 MB\n");
    kputs("  inodes used : "); kput_dec(ui); kputs(" / 256\n");
    fs_getcwd(cwdbuf, sizeof cwdbuf);
    kputs("  current dir : "); kputs(cwdbuf); kput('\n');
    g_file_count = 0;
    fs_list(count_cb);
    kputs("  entries here: "); kput_dec((unsigned)g_file_count); kput('\n');
}

static void cmd_format(void)
{
    if (!is_root()) { kputs("format: permission denied (root only)\n"); return; }
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
    else if(r == -4) kputs("rmdir: permission denied\n");
}

static void cmd_rd(const char *name)
{
    if(!name || !*name){ kputs("usage: rd <dir>   (force delete, even non-empty)\n"); return; }
    if(!fs_is_ready()){ kputs("NovaFS not formatted.\n"); return; }
    int r = fs_rmtree(name);
    if(r == -1)      kputs("rd: no such directory\n");
    else if(r == -2) kputs("rd: not a directory (use rm for files)\n");
    else if(r == -3) kputs("rd: cannot delete cwd or its ancestor\n");
    else if(r == -4) kputs("rd: permission denied\n");
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
    if(fs_read(name, (uint8_t*)textbuf, TXT_MAX) == -2){
        kputs("cat: permission denied\n");
        return;
    }
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
    if(w == -2){ kputs("Write failed: permission denied (not the owner)\n"); return; }
    if(w < 0){ kputs("Write failed\n"); return; }
    kputs("Saved "); kput_dec((unsigned)w); kputs(" bytes.\n");
}

static void cmd_rm(const char *name)
{
    if(!name || !*name){ kputs("usage: rm <file>\n"); return; }
    if(!fs_is_ready()){ kputs("NovaFS not formatted.\n"); return; }
    int r = fs_remove(name);
    if(r == -2)      kputs("rm: permission denied\n");
    else if(r < 0)   kputs("rm: failed (is it a directory? use rmdir)\n");
    else             kputs("Deleted.\n");
}

/* chmod FILE MODE - MODE = two octal digits (owner, others), each 0-7
 * with bits r=4 w=2 x=1. E.g. "64" = rw-r--, "75" = rwxr-x, "60" = rw---- */
static void cmd_chmod(const char *args)
{
    if(!args || !*args){
        kputs("usage: chmod FILE MODE   (MODE = two octal digits: owner, others; e.g. 64 = rw-r--)\n");
        return;
    }
    if(!fs_is_ready()){ kputs("NovaFS not formatted.\n"); return; }
    char name[FS_NAME_LEN];
    int k = 0;
    const char *s = args;
    while(*s == ' ') s++;
    while(*s && *s != ' ' && k < FS_NAME_LEN - 1) name[k++] = *s++;
    name[k] = 0;
    while(*s == ' ') s++;
    if(!name[0] || !s[0] || !s[1] || s[2] ||
       s[0] < '0' || s[0] > '7' || s[1] < '0' || s[1] > '7'){
        kputs("usage: chmod FILE MODE   (MODE = two octal digits, e.g. 64)\n");
        return;
    }
    uint8_t mode = (uint8_t)(((s[0] - '0') << 4) | (s[1] - '0'));
    if(!mode){
        kputs("chmod: mode 00 not allowed (00 means \"legacy default\" on disk)\n");
        return;
    }
    int r = fs_chmod(name, mode);
    if(r == -2)      kputs("chmod: permission denied (not the owner)\n");
    else if(r < 0)   kputs("chmod: no such file\n");
    else             kputs("Mode set.\n");
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
    else if (str_eq(cmd, "passwd"))cmd_passwd();
    else if (str_eq(cmd, "useradd"))cmd_useradd(args);
    else if (str_eq(cmd, "userdel"))cmd_userdel(args);
    else if (str_eq(cmd, "su"))     cmd_su(args);
    else if (str_eq(cmd, "whoami")) { kputs(g_cur_user); kput('\n'); }
    else if (str_eq(cmd, "logout"))cmd_logout();
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
    else if (str_eq(cmd, "chmod")) cmd_chmod(args);
    else if (str_eq(cmd, "feature") || str_eq(cmd, "feture")) cmd_feature(args);
    else if (str_eq(cmd, "run"))   cmd_run(args);
    else if (str_eq(cmd, "procs")) cmd_procs();
    else if (str_eq(cmd, "fg"))    cmd_fg();
    else if (str_eq(cmd, "kill"))  cmd_kill(args);
    else if (str_eq(cmd, "ps"))    cmd_procs();
    else if (str_eq(cmd, "mkdemo"))cmd_mkdemo();
    else if (str_eq(cmd, "netinfo"))cmd_netinfo();
    else if (str_eq(cmd, "dhcp"))  cmd_net_dhcp();
    else if (str_eq(cmd, "ping"))  cmd_net_ping(args);
    else if (str_eq(cmd, "dns"))   cmd_net_dns(args);
    else if (str_eq(cmd, "wget"))  cmd_wget(args);
    else if (str_eq(cmd, "udpecho"))cmd_net_udpecho();
    else { kputs("Unknown command: "); kputs(cmd); kputs("  (try 'help')\n"); }
}

static void shell_run(void) {
    for (;;) {
        int cmdcur = 0;                          /* cursor inside cmdline */
        cmdlen = 0;
        shell_prompt();
        for (;;) {
            char c = kb_read();
            if (c == K_LEFT) {                   /* cursor left (no erase) */
                if (cmdcur > 0) { cmdcur--; kput('\v'); }
            } else if (c == K_RIGHT) {           /* cursor right */
                if (cmdcur < cmdlen) { kput(cmdline[cmdcur]); cmdcur++; }
            } else if (c == '\n') { kput('\n'); break; }
            else if (c == '\b') {                /* delete before cursor */
                if (cmdcur > 0) {
                    for (int i = cmdcur - 1; i < cmdlen - 1; i++) cmdline[i] = cmdline[i + 1];
                    cmdlen--; cmdcur--; kput('\b');
                    for (int i = cmdcur; i < cmdlen; i++) kput(cmdline[i]);
                    for (int i = cmdcur; i < cmdlen; i++) kput('\v');
                }
            } else if ((uint8_t)c < 0x20) continue;              /* other control keys */
            else if (cmdlen < CMD_MAX - 1) {     /* insert at the cursor */
                for (int i = cmdlen; i > cmdcur; i--) cmdline[i] = cmdline[i - 1];
                cmdline[cmdcur++] = c; cmdlen++; kput(c);
                for (int i = cmdcur; i < cmdlen; i++) kput(cmdline[i]);
                for (int i = cmdcur; i < cmdlen; i++) kput('\v');
            }
        }
        process_cmd();
        if (g_logout) return;                    /* back to login */
    }
}

/* ============================================================
 * Login: root is implicit and PASSWORDLESS (no /passwd line = no prompt).
 * 'passwd' as root sets one; an empty new password removes it again.
 * ============================================================ */
#define LOGIN_USER     "root"
#define PASS_MAX       32

/* privilege model: root manages accounts + formats; everyone else may not.
 * (No per-user file permissions yet — one step at a time.) */
static int is_root(void) { return str_eq(g_cur_user, LOGIN_USER); }

static void login_run(void);
static void cmd_passwd(void);
static void cmd_logout(void);
static void cmd_useradd(const char *name);
static void cmd_userdel(const char *name);
static void cmd_su(const char *name);

static uint32_t pass_hash(const char *s)
{
    uint32_t h = 2166136261u;                    /* FNV-1a */
    while (*s) { h ^= (uint8_t)*s++; h *= 16777619u; }
    return h;
}

/* numeric owner id for NovaFS permission checks (root = 0, which is also
 * what every inode on pre-permission disks carries) */
static uint8_t user_uid(const char *name)
{
    if (str_eq(name, "root")) return 0;
    uint8_t u = (uint8_t)(pass_hash(name) & 0xFF);
    return u ? u : 1;
}

static int hex_val(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

/* cd to an absolute path like "/a/b" (best effort) */
static void fs_cd_path(const char *path)
{
    char seg[FS_NAME_LEN + 1];
    fs_cd("/");
    int i = 0;
    while (path[i]) {
        while (path[i] == '/') i++;
        int n = 0;
        while (path[i] && path[i] != '/') {
            if (n < FS_NAME_LEN) seg[n++] = path[i];
            i++;
        }
        if (n) {
            seg[n] = 0;
            if (fs_cd(seg) < 0) return;          /* path vanished */
        }
    }
}

/* ---- account database: /passwd, one line per user "name:HHHHHHHH\n" ---- */

static void hex8(char *dst, uint32_t h)
{
    static const char *hex = "0123456789ABCDEF";
    for (int i = 0; i < 8; i++)
        dst[i] = hex[(h >> (28 - i * 4)) & 0xF];
}

/* read /passwd into textbuf, return byte count (0 = missing/empty,
 * -1 = too big to handle — callers must not rewrite it) */
static int passwd_load(void)
{
    for (int i = 0; i < TXT_MAX; i++) textbuf[i] = 0;
    int sz = fs_size("passwd");
    if (sz <= 0) return 0;
    if (sz >= TXT_MAX) return -1;
    fs_read("passwd", (uint8_t*)textbuf, TXT_MAX);
    return sz;
}

/* look up a user; returns 1 and stores hash when found.
 * Passwordless root: root is implicit and normally has NO line in
 * /passwd - that state means "no password" (login skips the prompt).
 * If a root line exists it must match exactly. */
static int login_find_hash(const char *user, uint32_t *out)
{
    int len = str_len(user);
    if (fs_is_ready()) {
        fs_getcwd(cwdbuf, sizeof(cwdbuf));
        fs_cd("/");
        int sz = passwd_load();
        fs_cd_path(cwdbuf);
        for (int i = 0; i < sz; ) {
            int j = i;
            while (j < sz && textbuf[j] != '\n') j++;
            /* line [i,j): "name:HEX8" */
            if (j - i == len + 1 + 8 && textbuf[i + len] == ':') {
                int m = 0;
                while (m < len && textbuf[i + m] == user[m]) m++;
                if (m == len) {
                    uint32_t v = 0;
                    int bad = 0;
                    for (int k = 0; k < 8; k++) {
                        int d = hex_val(textbuf[i + len + 1 + k]);
                        if (d < 0) { bad = 1; break; }
                        v = (v << 4) | (uint32_t)d;
                    }
                    if (bad) return 0;   /* corrupted line: never grant access */
                    *out = v;
                    return 1;
                }
            }
            i = j + 1;
        }
    }
    return 0;    /* root with no line = passwordless (handled by callers) */
}

/* 1 = this account logs in WITHOUT a password (root with no /passwd line) */
static int login_is_passwordless(const char *user)
{
    uint32_t h;
    if (!str_eq(user, LOGIN_USER)) return 0;   /* only root may be passwordless */
    return !login_find_hash(user, &h);         /* no line = no password */
}

/* add or update one user line, drop every other line for that name */
static int login_set_user(const char *user, uint32_t h)
{
    if (!fs_is_ready()) return -1;
    fs_getcwd(cwdbuf, sizeof(cwdbuf));
    fs_cd("/");
    int sz = passwd_load();
    if (sz < 0) { fs_cd_path(cwdbuf); return -1; }   /* oversize: don't clobber it */
    /* compact /passwd IN PLACE: the write pointer never passes the read
     * pointer, so no second buffer is needed (a fresh[TXT_MAX] buffer once
     * pushed kernel BSS into the page-table area at 0x141000 and froze
     * the boot inside ring3_init) */
    int fn = 0, len = str_len(user);
    for (int i = 0; i < sz; ) {
        int j = i;
        while (j < sz && textbuf[j] != '\n') j++;
        int skip = (j - i == len + 1 + 8 && textbuf[i + len] == ':');
        int m = 0;
        while (skip && m < len && textbuf[i + m] == user[m]) m++;
        if (skip && m == len) { i = j + 1; continue; }   /* old line for user */
        while (i <= j && fn < TXT_MAX - 1) textbuf[fn++] = textbuf[i++];
    }
    if (fn + len + 10 >= TXT_MAX) { fs_cd_path(cwdbuf); return -1; }
    for (int k = 0; k < len; k++) textbuf[fn++] = user[k];
    textbuf[fn++] = ':';
    hex8(&textbuf[fn], h); fn += 8;
    textbuf[fn++] = '\n';
    int w = fs_write("passwd", (uint8_t*)textbuf, (uint32_t)fn);
    fs_cd_path(cwdbuf);
    return w;
}

/* remove a user line (keeps others) */
static int login_del_user(const char *user)
{
    if (!fs_is_ready()) return -1;
    fs_getcwd(cwdbuf, sizeof(cwdbuf));
    fs_cd("/");
    int sz = passwd_load(), found = 0;
    if (sz < 0) { fs_cd_path(cwdbuf); return -1; }   /* oversize: don't clobber it */
    /* in-place compaction, same as login_set_user (no second buffer) */
    int fn = 0, len = str_len(user);
    for (int i = 0; i < sz; ) {
        int j = i;
        while (j < sz && textbuf[j] != '\n') j++;
        int hit = (j - i == len + 9 && textbuf[i + len] == ':');
        int m = 0;
        while (hit && m < len && textbuf[i + m] == user[m]) m++;
        if (hit && m == len) { found = 1; i = j + 1; continue; }
        while (i <= j && fn < TXT_MAX - 1) textbuf[fn++] = textbuf[i++];
    }
    int w = found ? fs_write("passwd", (uint8_t*)textbuf, (uint32_t)fn) : -1;
    fs_cd_path(cwdbuf);
    return w;
}

/* line input with backspace; echoes '*' when masked */
static void read_line(char *buf, int max, int masked)
{
    int len = 0;
    for (;;) {
        char c = kb_read();
        if ((uint8_t)c < 0x20 && c != '\n' && c != '\b') continue;
        if (c == '\n') { kput('\n'); break; }
        if (c == '\b') { if (len > 0) { len--; kput('\b'); } }
        else if (len < max - 1) { buf[len++] = c; kput(masked ? '*' : c); }
    }
    buf[len] = 0;
}

static void login_run(void)
{
    char user[FS_NAME_LEN + 1];
    char pass[PASS_MAX];
    uint32_t h;
    for (;;) {
        kput('\n');
        set_color(g_accent); vga_puts("NovaOS login: "); reset_color();
        serial_puts("\nNovaOS login: ");
        read_line(user, sizeof(user), 0);
        serial_puts(user); serial_putc('\n');
        if (login_is_passwordless(user)) {
            /* root without a /passwd line: no password prompt at all */
            for (int i = 0; i < (int)sizeof(g_cur_user); i++)
                g_cur_user[i] = user[i];
            fs_setuid(user_uid(user));
            fs_cd("/");
            kputs("\nWelcome, ");
            set_color(C_LGREEN); kputs(g_cur_user); reset_color();
            kputs(". Type 'help' for commands.\n");
            return;
        }
        set_color(g_accent); vga_puts("Password: "); reset_color();
        serial_puts("Password: ");
        read_line(pass, sizeof(pass), 1);
        serial_putc('\n');
        if (login_find_hash(user, &h) && pass_hash(pass) == h) {
            for (int i = 0; i < (int)sizeof(g_cur_user); i++)
                g_cur_user[i] = user[i];
            fs_setuid(user_uid(user));       /* file ownership follows login */
            /* land in the user's home directory (root stays at /) */
            fs_cd("/");
            if (!str_eq(user, LOGIN_USER) && fs_cd("home") == 0) {
                if (fs_cd(user) < 0) fs_cd("/");
            }
            kputs("\nWelcome, ");
            set_color(C_LGREEN); kputs(g_cur_user); reset_color();
            kputs(". Type 'help' for commands.\n");
            return;
        }
        set_color(C_LRED); kputs("Login incorrect.\n"); reset_color();
    }
}

static void cmd_logout(void)
{
    kputs("Goodbye.\n");
    g_logout = 1;
}

static void cmd_passwd(void)
{
    if (!fs_is_ready()) { kputs("passwd: NovaFS not formatted, cannot save\n"); return; }
    char oldp[PASS_MAX], p1[PASS_MAX], p2[PASS_MAX];
    if (login_is_passwordless(g_cur_user)) {
        kputs("passwd: root has no password - leave the new one empty to keep it that way\n");
    } else {
        set_color(g_accent); vga_puts("Old password: "); reset_color();
        read_line(oldp, sizeof(oldp), 1);
        uint32_t h;
        if (!login_find_hash(g_cur_user, &h) || pass_hash(oldp) != h) {
            set_color(C_LRED); kputs("passwd: wrong password\n"); reset_color();
            return;
        }
    }
    set_color(g_accent); vga_puts("New password: "); reset_color();
    read_line(p1, sizeof(p1), 1);
    if (!p1[0]) {
        /* empty new password: root -> drop the line (passwordless again);
         * normal users keep their mandatory password */
        if (str_eq(g_cur_user, LOGIN_USER)) {
            if (login_del_user(LOGIN_USER) < 0) { kputs("passwd: already passwordless\n"); return; }
            kputs("passwd: root password removed (passwordless login on)\n");
            return;
        }
        kputs("passwd: empty password not allowed\n");
        return;
    }
    set_color(g_accent); vga_puts("Retype new password: "); reset_color();
    read_line(p2, sizeof(p2), 1);
    if (!str_eq(p1, p2)) { kputs("passwd: passwords do not match\n"); return; }
    if (login_set_user(g_cur_user, pass_hash(p1)) < 0) {
        set_color(C_LRED); kputs("passwd: write failed\n"); reset_color();
        return;
    }
    kputs("Password updated.\n");
}

static void cmd_useradd(const char *name)
{
    if (!fs_is_ready()) { kputs("useradd: NovaFS not formatted\n"); return; }
    if (!is_root()) { kputs("useradd: permission denied (root only)\n"); return; }
    if (!name || !*name) { kputs("usage: useradd <name>\n"); return; }
    if (str_len(name) >= FS_NAME_LEN) { kputs("useradd: name too long\n"); return; }
    for (const char *p = name; *p; p++)
        if (*p == ':' || *p == '/') { kputs("useradd: name may not contain ':' or '/'\n"); return; }
    uint32_t h;
    if (login_find_hash(name, &h)) { kputs("useradd: user already exists\n"); return; }
    char p1[PASS_MAX], p2[PASS_MAX];
    set_color(C_LCYAN); vga_puts("New password: "); reset_color();
    read_line(p1, sizeof(p1), 1);
    if (!p1[0]) { kputs("useradd: empty password not allowed\n"); return; }
    set_color(C_LCYAN); vga_puts("Retype new password: "); reset_color();
    read_line(p2, sizeof(p2), 1);
    if (!str_eq(p1, p2)) { kputs("useradd: passwords do not match\n"); return; }
    if (login_set_user(name, pass_hash(p1)) < 0) {
        set_color(C_LRED); kputs("useradd: write failed\n"); reset_color();
        return;
    }
    /* home directory /home/<name> (best effort) */
    fs_getcwd(cwdbuf, sizeof(cwdbuf));
    fs_cd("/");
    if (fs_find("home") < 0) fs_mkdir("home");
    if (fs_cd("home") == 0 && fs_find(name) < 0) fs_mkdir(name);
    fs_cd_path(cwdbuf);
    kputs("User '"); kputs(name); kputs("' created.\n");
}

static void cmd_userdel(const char *name)
{
    if (!fs_is_ready()) { kputs("userdel: NovaFS not formatted\n"); return; }
    if (!is_root()) { kputs("userdel: permission denied (root only)\n"); return; }
    if (!name || !*name) { kputs("usage: userdel <name>\n"); return; }
    if (str_eq(name, LOGIN_USER)) { kputs("userdel: cannot delete root\n"); return; }
    if (login_del_user(name) < 0) { kputs("userdel: no such user\n"); return; }
    /* drop the home directory too (best effort) */
    fs_getcwd(cwdbuf, sizeof(cwdbuf));
    fs_cd("/");
    if (fs_cd("home") == 0 && fs_rmtree(name) == 0)
        kputs("Home directory removed.\n");
    fs_cd_path(cwdbuf);
    kputs("User '"); kputs(name); kputs("' deleted.\n");
}

/* su <name>: root switches to anyone without a password; anyone else must
 * enter the target user's password. Single session — the old identity is
 * simply replaced (there is no `exit` to go back; su again to return). */
static void cmd_su(const char *name)
{
    char pass[PASS_MAX];
    uint32_t h;
    if (!fs_is_ready()) { kputs("su: NovaFS not formatted\n"); return; }
    if (!name || !*name) { kputs("usage: su <user>\n"); return; }
    if (str_len(name) >= FS_NAME_LEN) { kputs("su: no such user\n"); return; }
    if (str_eq(name, g_cur_user)) { kputs("su: already this user\n"); return; }
    if (login_is_passwordless(name)) {
        /* su into root: passwordless root only trusts the login screen,
         * otherwise passwordless login would be a privilege-escalation
         * backdoor for every account */
        kputs("su: root has no password - set one first ('passwd' as root)\n");
        return;
    }
    if (!login_find_hash(name, &h)) { kputs("su: no such user\n"); return; }
    if (!is_root()) {
        set_color(g_accent); vga_puts("Password: "); reset_color();
        serial_puts("Password: ");
        read_line(pass, sizeof(pass), 1);
        serial_putc('\n');
        if (pass_hash(pass) != h) {
            set_color(C_LRED); kputs("su: wrong password\n"); reset_color();
            return;
        }
    }
    for (int i = 0; i < (int)sizeof(g_cur_user); i++)
        g_cur_user[i] = name[i];
    fs_setuid(user_uid(name));           /* file ownership follows su */
    /* land in the new user's home directory (root stays at /) */
    fs_cd("/");
    if (!str_eq(name, LOGIN_USER) && fs_cd("home") == 0) {
        if (fs_cd(name) < 0) fs_cd("/");
    }
    kputs("Now logged in as ");
    set_color(C_LGREEN); kputs(g_cur_user); reset_color();
    kput('\n');
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

/* serial-only gfx state dump (works even when the LFB console is dead) */
static void gfx_probe(const char *tag)
{
    static const char *hx = "0123456789ABCDEF";
    uint32_t v;
    serial_puts("[gfxdbg] "); serial_puts(tag);
    serial_puts(" state=0x");
    v = gfx_dbg_state();
    for (int i = 28; i >= 0; i -= 4) serial_putc(hx[(v >> i) & 0xF]);
    serial_puts(" fb=0x");
    v = (uint32_t)(unsigned long)gfx_dbg_fb();
    for (int i = 28; i >= 0; i -= 4) serial_putc(hx[(v >> i) & 0xF]);
    serial_putc('\n');
}

void kmain(void) {
    serial_init();
    int gfx = gfx_init();      /* capture font, find LFB, set VBE mode */
    int fr = fs_init();        /* mount BEFORE the banner: nova.cfg drives
                                * accent + quiet, and quiet hides the rest */
    cfg_load();
    feature_init();            /* /etc/features.conf - module kill switches */
    if (!g_quiet) {
        banner();
        gfx_probe("after-banner");
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
    }

    if (!g_quiet) {
        if (fr == 0)      boot_tag(1, "novafs", "mounted");
        else if (fr == 1) boot_tag(1, "novafs", "fresh disk, auto-formatted");
        else              boot_tag(0, "novafs", "disk I/O error");
        boot_tag(1, "feat",  "feature subsystem (/etc/features.conf)");
    }

    if (acpi_init() == 0) {
        if (!g_quiet) {
            if (g_acpi.s5_found) boot_tag(1, "acpi", "\\_S5 found, poweroff ready");
            else                 boot_tag(1, "acpi", "tables found");
        }
    } else if (!g_quiet) {
        boot_tag(0, "acpi", "tables not found");
    }
    int mse = mouse_init();
    if (!g_quiet) {
        boot_tag(mse == 0, "input",  mse == 0 ? "keyboard + PS/2 mouse"
                                              : "keyboard (no aux mouse)");
    }
    ring3_init(&nxp_api);
    if (!g_quiet) boot_tag(1, "ring3", "paging + TSS + IDT, user mode ready");
    int net = net_init();      /* probe NIC - needs live page tables for MMIO */
    if (!g_quiet) boot_tag(net, "net", net ? "e1000 + TCP/IP stack ready"
                                           : "no NIC found (network off)");
    nxp_api.scr_w = gfx_active() ? (uint32_t)(gfx_cols() * 8) : 0;
    nxp_api.scr_h = gfx_active() ? (uint32_t)(gfx_rows() * 16) : 0;
    ring3_setup_tramp();                       /* syscall stubs, once */

    kput('\n');
    login_run();
    for (;;) {
        g_logout = 0;
        shell_run();                             /* returns on logout */
        login_run();
    }
}
