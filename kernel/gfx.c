/* ============================================================
 * NovaOS - graphics driver implementation (see gfx.h)
 * ============================================================ */
#include "stdint.h"
#include "gfx.h"
#include "cnfont.h"

static inline void outb_(uint16_t p, uint8_t v)
{
    __asm__ volatile ("outb %0, %1" : : "a"(v), "Nd"(p));
}
static inline void outw_(uint16_t p, uint16_t v)
{
    __asm__ volatile ("outw %0, %1" : : "a"(v), "Nd"(p));
}
static inline uint8_t inb_(uint16_t p)
{
    uint8_t v; __asm__ volatile ("inb %1, %0" : "=a"(v) : "Nd"(p)); return v;
}
static inline uint16_t inw_(uint16_t p)
{
    uint16_t v; __asm__ volatile ("inw %1, %0" : "=a"(v) : "Nd"(p)); return v;
}

/* ---------- VGA 8x16 font capture (must run in text mode) ---------- */
#define GLYPH_H 16
#define GLYPH_W 8
static uint8_t font[256][GLYPH_H];           /* 4 KiB */

static int fail_stage = 0;   /* diagnostics: 1=font 2=pci 3=vbe */
static uint16_t dbg_id;
static uint32_t dbg_bar;
int gfx_fail_stage(void) { return fail_stage; }
uint16_t gfx_dbg_id(void)  { return dbg_id; }
uint32_t gfx_dbg_bar(void) { return dbg_bar; }
const uint8_t *gfx_dbg_font(int c) { return font[c & 0xFF]; }

static void capture_font(void)
{
    volatile uint8_t *plane2 = (volatile uint8_t *)0xA0000;

    outb_(0x3C4, 0x04); uint8_t seq4 = inb_(0x3C5);
    outb_(0x3CE, 0x05); uint8_t grc5 = inb_(0x3CF);
    outb_(0x3CE, 0x04); uint8_t grc4 = inb_(0x3CF);
    outb_(0x3CE, 0x06); uint8_t grc6 = inb_(0x3CF);
    uint8_t misc = inb_(0x3CC);

    /* sequencer: chained/odd-even addressing off, extended memory on */
    outb_(0x3C4, 0x04); outb_(0x3C5, (uint8_t)((seq4 & ~0x03) | 0x04));
    /* graphics controller: read plane 2, read mode 0, odd-even off */
    outb_(0x3CE, 0x04); outb_(0x3CF, 0x02);
    outb_(0x3CE, 0x05); outb_(0x3CF, (uint8_t)(grc5 & ~0x10));
    /* memory map: 64K window at 0xA0000. QEMU's mapping honors GRC6 bits[3:2]
     * (real HW also wants misc 0x3C2 bits[3:2] - set both) */
    outb_(0x3CE, 0x06); outb_(0x3CF, (uint8_t)((grc6 & ~0x0C) | 0x04));
    outb_(0x3C2, (uint8_t)((misc & ~0x0C) | 0x04));

    /* font plane: 256 glyphs, 32-byte stride, glyph rows in the first 16 */
    for (int c = 0; c < 256; c++)
        for (int r = 0; r < GLYPH_H; r++)
            font[c][r] = plane2[c * 32 + r];

    outb_(0x3C4, 0x04); outb_(0x3C5, seq4);
    outb_(0x3CE, 0x04); outb_(0x3CF, grc4);
    outb_(0x3CE, 0x05); outb_(0x3CF, grc5);
    outb_(0x3CE, 0x06); outb_(0x3CF, grc6);
    outb_(0x3C2, misc);
}

/* ---------- PCI: find display controller, return BAR0 ---------- */
static uint32_t pci_read32(uint8_t dev, uint8_t off)
{
    uint32_t addr = 0x80000000u | ((uint32_t)dev << 11) | off;
    __asm__ volatile ("outl %0, %1" : : "a"(addr), "Nd"(0xCF8));
    uint32_t v;
    __asm__ volatile ("inl %1, %0" : "=a"(v) : "Nd"(0xCFC));
    return v;
}

static uint32_t find_vga_lfb(void)
{
    for (int dev = 0; dev < 32; dev++) {
        if (pci_read32((uint8_t)dev, 0x00) == 0xFFFFFFFFu) continue;  /* empty */
        uint32_t cls = pci_read32((uint8_t)dev, 0x08);
        if (((cls >> 24) & 0xFF) != 0x03) continue;                   /* display */
        uint32_t bar = pci_read32((uint8_t)dev, 0x10);
        if (bar & 1) continue;                                        /* IO BAR */
        bar &= 0xFFFFFFF0u;
        if (bar) return bar;
    }
    return 0;
}

/* ---------- Bochs VBE register programming ---------- */
#define VBE_IDX 0x1CE
#define VBE_DAT 0x1CF

#define VBE_REG_ID     0x00
#define VBE_REG_XRES   0x01
#define VBE_REG_YRES   0x02
#define VBE_REG_BPP    0x03
#define VBE_REG_ENABLE 0x04

static void vbe_wr(uint8_t idx, uint16_t val)
{
    outw_(VBE_IDX, idx);          /* index register is 16-bit: use word I/O */
    outw_(VBE_DAT, val);
}
static uint16_t vbe_rd(uint8_t idx)
{
    outw_(VBE_IDX, idx);
    return inw_(VBE_DAT);
}

static int vbe_set_mode(int x, int y, int bpp)
{
    uint16_t id = vbe_rd(VBE_REG_ID);
    dbg_id = id;
    if ((id & 0xFFF0) != 0xB0C0) return -1;   /* no Bochs VBE extension */
    vbe_wr(VBE_REG_ENABLE, 0x0000);           /* disable while reconfiguring */
    vbe_wr(VBE_REG_XRES,  (uint16_t)x);
    vbe_wr(VBE_REG_YRES,  (uint16_t)y);
    vbe_wr(VBE_REG_BPP,   (uint16_t)bpp);
    vbe_wr(VBE_REG_ENABLE, 0x0003);           /* enable + linear framebuffer */
    /* read back what the card actually accepted */
    dbg_bar = ((uint32_t)vbe_rd(VBE_REG_XRES) << 16) | vbe_rd(VBE_REG_YRES);
    dbg_id  = (vbe_rd(VBE_REG_BPP) << 16) | vbe_rd(VBE_REG_ENABLE);
    return 0;
}

/* ---------- LFB console ---------- */
#define SCR_W   1024
#define SCR_H   768
#define SCR_BPP 32

static const uint32_t pal[16] = {
    0x00000000u, 0x000000AAu, 0x0000AA00u, 0x0000AAAAu,   /* black blue green cyan */
    0x00AA0000u, 0x00AA00AAu, 0x00AA5500u, 0x00AAAAAAu,   /* red magenta brown lgray */
    0x00555555u, 0x005555FFu, 0x0055FF55u, 0x0055FFFFu,   /* dgray lblue lgreen lcyan */
    0x00FF5555u, 0x00FF55FFu, 0x00FFFF55u, 0x00FFFFFFu,   /* lred lmagenta yellow white */
};

static volatile uint32_t *fb;      /* LFB as 32bpp pixels */
static uint32_t lfb_base;          /* raw PCI BAR of the LFB (paging maps it), 0 = none */
static int con_cols, con_rows;
static int cur_x, cur_y;
static uint8_t fg_idx_cur = 7, bg_idx_cur = 0;
static uint32_t fg_col = pal[7], bg_col = pal[0];
static int gfx_on = 0;

/* ---- scrollback: cell mirror of the live screen + ring of scrolled-off
 * lines. Packed cell = (bg<<12)|(fg<<8)|char. The wheel (kernel shell)
 * views older lines; any key returns to the live view. VGA text fallback
 * has no scrollback (gfx-only feature). ---- */
#define SB_N     100            /* remembered off-screen lines */
#define SB_COLS  128
#define SB_PACK(fg, bg, ch) ((unsigned short)((bg) << 12 | (fg) << 8 | (unsigned char)(ch)))
#define SB_CH(c)   ((char)((c) & 0xFF))
#define SB_FG(c)   ((uint8_t)(((c) >> 8) & 0xF))
#define SB_BG(c)   ((uint8_t)(((c) >> 12) & 0xF))
static unsigned short sb_ring[SB_N][SB_COLS];   /* scrolled-off lines */
static int sb_count;                            /* valid ring entries */
static int sb_head;                             /* ring index of newest */
static unsigned short sb_view[48][SB_COLS];     /* live screen mirror */
static unsigned short sb_blank[SB_COLS];        /* older-than-memory rows */
static int sb_off;                              /* 0 = live, N = N lines up */

static inline void px(int x, int y, uint32_t c);
extern void serial_puts(const char *s);
extern void serial_putc(char c);

static void sb_clear_tail(int cy, int from_x);
static void sb_push_top(void)
{
    sb_head = (sb_head + 1) % SB_N;
    for (int c = 0; c < SB_COLS; c++) sb_ring[sb_head][c] = sb_view[0][c];
    if (sb_count < SB_N) sb_count++;
    for (int r = 1; r < 48; r++)
        for (int c = 0; c < SB_COLS; c++) sb_view[r - 1][c] = sb_view[r][c];
    sb_clear_tail(47, 0);                        /* scrolled-in row is blank */
}

static void sb_capture(int cx, int cy, char ch)
{
    if (cx < 0 || cx >= SB_COLS || cy < 0 || cy >= 48) return;
    sb_view[cy][cx] = SB_PACK(fg_idx_cur, bg_idx_cur, ch);
}

/* blank the tail of a mirror row from cx (line complete on newline) */
static void sb_clear_tail(int cy, int from_x)
{
    if (cy < 0 || cy >= 48) return;
    for (int cx = from_x; cx < SB_COLS; cx++)
        sb_view[cy][cx] = SB_PACK(fg_idx_cur, bg_idx_cur, ' ');
}

static void sb_blit_line(int row, const unsigned short *cells)
{
    int px_y = row * GLYPH_H;
    for (int cx = 0; cx < con_cols && cx < SB_COLS; cx++) {
        unsigned short cell = cells[cx];
        const uint8_t *g = font[(unsigned char)SB_CH(cell)];
        uint32_t fg = pal[SB_FG(cell)], bg = pal[SB_BG(cell)];
        int px_x = cx * GLYPH_W;
        for (int r = 0; r < GLYPH_H; r++) {
            uint8_t bits = g[r];
            for (int b = 0; b < GLYPH_W; b++)
                px(px_x + b, px_y + r, (bits & (0x80 >> b)) ? fg : bg);
        }
    }
}

/* scroll the view: delta > 0 = view older lines, < 0 = newer; 0 = live.
 * History lines are blitted over the screen; returning to live redraws
 * every row from the mirror (sb_view), which putc/gfx_cell keep current. */
void gfx_sb_scroll(int delta)
{
    if (!gfx_on) return;
    int off = sb_off + delta;
    if (off < 0) off = 0;
    if (off > sb_count) off = sb_count;
    if (off == sb_off) return;
    if (sb_off == 0 && delta > 0) {
        /* DEBUG: dump the mirror text to serial on the first wheel-up */
        for (int r = 0; r < 48; r++) {
            serial_puts("[sb] ");
            for (int c = 0; c < 100 && c < SB_COLS; c++) {
                char ch = SB_CH(sb_view[r][c]);
                serial_putc(ch >= ' ' && ch < 127 ? ch : '.');
            }
            serial_putc('\n');
        }
    }
    sb_off = off;
    if (sb_off == 0) {                       /* redraw the live screen */
        for (int row = 0; row < 48; row++) sb_blit_line(row, sb_view[row]);
        gfx_move_cursor();
        return;
    }
    for (int row = 0; row < 48; row++) {
        int back = sb_off + (47 - row);      /* lines back from the bottom */
        if (back < 48) { sb_blit_line(row, sb_view[47 - back]); continue; }
        int ring_back = back - 48;           /* 0 = newest ring entry */
        if (ring_back >= sb_count) {
            for (int c = 0; c < SB_COLS; c++) sb_blank[c] = SB_PACK(0, 0, ' ');
            sb_blit_line(row, sb_blank);     /* older than memory: blank */
            continue;
        }
        int idx = (sb_head - ring_back + SB_N * 4) % SB_N;
        sb_blit_line(row, sb_ring[idx]);
    }
}

int gfx_active(void) { return gfx_on; }
int gfx_cols(void)   { return con_cols; }
int gfx_rows(void)   { return con_rows; }

/* debug probe: are the console internals still sane? */
uint32_t gfx_dbg_state(void)
{
    return ((uint32_t)gfx_on << 24) | ((uint32_t)(cur_y & 0xFFF) << 12) |
           (uint32_t)(cur_x & 0xFFF);
}
const void *gfx_dbg_fb(void) { return (const void *)fb; }
uint32_t gfx_lfb(void) { return lfb_base; }

static inline void px(int x, int y, uint32_t c)
{
    if (x >= 0 && x < SCR_W && y >= 0 && y < SCR_H) fb[y * SCR_W + x] = c;
}

int gfx_ready(void) { return gfx_on; }

void gfx_pixel(int x, int y, uint32_t rgb)
{
    if (gfx_on) px(x, y, rgb);
}

uint32_t gfx_pixel_get(int x, int y)
{
    if (!gfx_on || x < 0 || x >= SCR_W || y < 0 || y >= SCR_H) return 0;
    return fb[y * SCR_W + x];
}

/* core glyph blit with a raw RGB foreground */
static void blit_core(int x0, int y0, char ch, uint32_t fg, int scale)
{
    const uint8_t *g = font[(uint8_t)ch];
    for (int r = 0; r < GLYPH_H; r++) {
        uint8_t bits = g[r];
        for (int b = 0; b < GLYPH_W; b++) {
            uint32_t c = (bits & (0x80 >> b)) ? fg : bg_col;
            if (scale == 1) {
                px(x0 + b, y0 + r, c);
            } else {
                for (int sy = 0; sy < scale; sy++)
                    for (int sx = 0; sx < scale; sx++)
                        px(x0 + b * scale + sx, y0 + r * scale + sy, c);
            }
        }
    }
}

void gfx_text(int x, int y, const char *s, uint32_t rgb)
{
    if (!gfx_on) return;
    while (*s) {
        blit_core(x, y, *s++, rgb, 1);
        x += GLYPH_W;
    }
}

void gfx_blit_char(int x0, int y0, char ch, uint8_t fg_idx, int scale)
{
    blit_core(x0, y0, ch, pal[fg_idx & 0x0F], scale);
}

/* ---------- boot logo (true-color drawn emblem) ----------
 * Design: a supernova star-burst for "Nova" - layered halo, 16 tapered
 * gradient rays, white-hot core, an orbit ring with a moon (the OS as
 * the world around it), plus a bold "NovaOS" wordmark with shadow. */

/* 256-scale sine, 0..90 degrees, rounded */
static const short SB_SIN[91] = {
    0,4,9,13,18,22,27,31,36,40,44,49,53,58,62,66,71,75,79,83,88,92,96,100,
    104,108,112,116,120,124,128,132,136,139,143,147,150,154,158,161,165,168,
    171,175,178,181,184,187,190,193,196,199,202,204,207,210,212,215,217,219,
    222,224,226,228,230,232,234,236,237,239,241,242,243,245,246,247,248,249,
    250,251,252,253,253,254,255,255,256,256,256
};
static int sb_sin(int deg)
{
    deg %= 360; if (deg < 0) deg += 360;
    int sign = 1;
    if (deg >= 180) { sign = -1; deg -= 180; }
    if (deg > 90) deg = 180 - deg;
    return sign * SB_SIN[deg];
}
static int sb_cos(int deg) { return sb_sin(deg + 90); }

static void fill_circle(int cx, int cy, int r, uint32_t col)
{
    for (int dy = -r; dy <= r; dy++)
        for (int dx = -r; dx <= r; dx++)
            if (dx * dx + dy * dy <= r * r) px(cx + dx, cy + dy, col);
}

static uint32_t ray_mix(uint32_t a, uint32_t b, int t, int len)
{
    int r = (int)(((a >> 16) & 0xFF) * (len - t) + ((b >> 16) & 0xFF) * t) / len;
    int g = (int)(((a >> 8) & 0xFF) * (len - t) + ((b >> 8) & 0xFF) * t) / len;
    int bl = (int)((a & 0xFF) * (len - t) + (b & 0xFF) * t) / len;
    return ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)bl;
}

static void ray(int cx, int cy, int deg, int len, int w0,
                uint32_t hot, uint32_t cool)
{
    int dx = sb_cos(deg), dy = sb_sin(deg);
    for (int t = 0; t <= len; t += 2) {
        int r = 1 + w0 * (len - t) / len;
        fill_circle(cx + (dx * t) / 256, cy + (dy * t) / 256, r,
                    ray_mix(hot, cool, t, len));
    }
}

/* draw only the set bits of a glyph in a raw color (no bg fill) */
static void blit_rgb(int x0, int y0, char ch, uint32_t rgb, int scale)
{
    const uint8_t *g = font[(unsigned char)ch];
    for (int r = 0; r < GLYPH_H; r++)
        for (int b = 0; b < GLYPH_W; b++)
            if (g[r] & (0x80 >> b))
                for (int sy = 0; sy < scale; sy++)
                    for (int sx = 0; sx < scale; sx++)
                        px(x0 + b * scale + sx, y0 + r * scale + sy, rgb);
}

void gfx_boot_logo(void)
{
    if (!gfx_on) return;

    /* full black canvas + top accent bar, self-contained layout */
    cur_x = 0; cur_y = 0;
    for (uint32_t i = 0; i < (uint32_t)SCR_W * SCR_H; i++) fb[i] = 0x000000;
    gfx_grad_bar();                              /* top bar, cur_y -> 1 */

    const int cx = 512, cy = 148;

    /* layered halo */
    fill_circle(cx, cy, 112, 0x04080E);
    fill_circle(cx, cy, 92,  0x081019);
    fill_circle(cx, cy, 72,  0x0D1B2A);
    fill_circle(cx, cy, 52,  0x12263C);

    /* orbit ring + moon (the OS as the world around the star) */
    for (int d = 0; d < 360; d++) {
        int x = cx + (152 * sb_cos(d)) / 256;
        int y = cy + (40  * sb_sin(d)) / 256;
        px(x, y, 0x1E4258); px(x + 1, y, 0x1A3A50);
        px(x, y + 1, 0x163448); px(x + 1, y + 1, 0x122C40);
    }
    fill_circle(cx + (152 * sb_cos(335)) / 256, cy + (40 * sb_sin(335)) / 256,
                4, 0x7FB8D8);

    /* rays: the diagonal four first (dimmer), then the cross four on top
     * so all eight read with equal weight */
    for (int k = 0; k < 4; k++)
        ray(cx, cy, 45 + k * 90, 92, 7, 0xC8ECFA, 0x14547A);
    for (int k = 0; k < 4; k++)
        ray(cx, cy, k * 90,     100, 8, 0xE4F8FF, 0x1B608A);
    for (int k = 0; k < 8; k++)
        ray(cx, cy, 22 + k * 45, 46, 4, 0x6FC8E8, 0x0E3E5C);

    /* white-hot core */
    fill_circle(cx, cy, 22, 0x0E3A58);
    fill_circle(cx, cy, 16, 0x28A0D8);
    fill_circle(cx, cy, 11, 0x9FE8FF);
    fill_circle(cx, cy, 7,  0xFFFFFF);

    /* sparkles */
    fill_circle(cx - 96, cy - 58, 2, 0x6FD8FF);
    fill_circle(cx + 84, cy - 70, 2, 0x4FB8E0);
    fill_circle(cx + 110, cy + 12, 2, 0x6FD8FF);
    fill_circle(cx - 112, cy + 26, 2, 0x3FA8D8);
    fill_circle(cx + 52, cy + 96, 2, 0x4FB8E0);

    /* wordmark: "NovaOS" at 4x with shadow + bold */
    const char *wm = "NovaOS";
    int scale = 4, gw = GLYPH_W * scale;         /* 32 px per glyph */
    int x0 = (SCR_W - 6 * gw) / 2, y0 = 292;
    for (int i = 0; wm[i]; i++)
        blit_rgb(x0 + i * gw + 5, y0 + 5, wm[i], 0x00345A, scale);
    for (int i = 0; wm[i]; i++) {
        blit_rgb(x0 + i * gw,     y0, wm[i], 0xEAF6FF, scale);
        blit_rgb(x0 + i * gw + 1, y0, wm[i], 0xEAF6FF, scale);
    }

    /* tagline + build date + accent underline */
    const char *tag = "a tiny 32-bit operating system";
    int tl = 0; while (tag[tl]) tl++;
    gfx_text((SCR_W - tl * GLYPH_W) / 2, 378, tag, 0x9FC8DC);
    const char *dt = __DATE__;
    int dl = 0; while (dt[dl]) dl++;
    gfx_text((SCR_W - dl * GLYPH_W) / 2, 400, dt, 0x5F7F92);
    for (int x = SCR_W / 2 - 150; x < SCR_W / 2 + 150; x++) {
        px(x, 428, 0x00B4E0); px(x, 429, 0x0090B8);
    }

    /* console cursor lands below the logo */
    cur_x = 0; cur_y = 30;
}

/* cell-addressed glyph with explicit per-cell colors (TUI primitives).
 * Bypasses the text console entirely: no cursor, no scrolling. */
void gfx_cell(int cx, int cy, char ch, uint8_t fg_idx, uint8_t bg_idx)
{
    if (!gfx_on || cx < 0 || cy < 0 || cx >= con_cols || cy >= con_rows) return;
    int x0 = cx * GLYPH_W, y0 = cy * GLYPH_H;
    const uint8_t *g = font[(unsigned char)ch];
    uint32_t fg = pal[fg_idx & 0x0F], bg = pal[bg_idx & 0x0F];
    for (int r = 0; r < GLYPH_H; r++) {
        uint8_t bits = g[r];
        for (int b = 0; b < GLYPH_W; b++)
            px(x0 + b, y0 + r, (bits & (0x80 >> b)) ? fg : bg);
    }
    /* keep the scrollback mirror consistent with fullscreen apps too */
    if (cx < SB_COLS && cy < 48) sb_view[cy][cx] = SB_PACK(fg_idx, bg_idx, ch);
}

extern const uint8_t *cn_glyph(unsigned short code);   /* cnfont.c */

/* ---- CJK 16x16 rendering (cn_lang_support phase 1, see cnfont.h) ----
 * ASCII advances 8px using the captured VGA font; a UTF-8 codepoint with
 * a bank glyph advances 16px as a full-width block. Caller owes one
 * cn_can_render() check. */
int cn_text16(int x, int y, const char *s, uint32_t rgb)
{
    if (!gfx_on || !s) return -1;
    int adv = 0;
    while (*s) {
        unsigned char c = (unsigned char)*s;
        if (c < 0x80) {
            const uint8_t *g = font[c];
            for (int r = 0; r < GLYPH_H; r++) {
                uint8_t bits = g[r];
                for (int b = 0; b < GLYPH_W; b++)
                    px(x + adv + b, y + r, (bits & (0x80 >> b)) ? rgb : bg_col);
            }
            adv += GLYPH_W;
            s++;
            continue;
        }
        unsigned short cp; int len;
        if ((c & 0xE0) == 0xC0 && (s[1] & 0xC0) == 0x80) {
            cp = ((unsigned short)(c & 0x1F) << 6) | (unsigned short)(s[1] & 0x3F);
            len = 2;
        } else if ((c & 0xF0) == 0xE0 && (s[1] & 0xC0) == 0x80 && (s[2] & 0xC0) == 0x80) {
            cp = ((unsigned short)(c & 0x0F) << 12)
               | ((unsigned short)(s[1] & 0x3F) << 6)
               | (unsigned short)(s[2] & 0x3F);
            len = 3;
        } else return adv > 0 ? adv : -1;
        const uint8_t *g = cn_glyph(cp);
        if (!g) return adv > 0 ? adv : -1;
        for (int r = 0; r < 16; r++) {
            uint16_t bits = (uint16_t)((g[r * 2] << 8) | g[r * 2 + 1]);
            for (int b = 0; b < 16; b++)
                px(x + adv + b, y + r, (bits & (0x8000 >> b)) ? rgb : bg_col);
        }
        adv += 16;
        s += len;
    }
    return adv;
}

static void draw_cell(int cx, int cy)
{
    /* full cell incl. background so it also erases a stale cursor */
    gfx_blit_char(cx * GLYPH_W, cy * GLYPH_H, ' ', 0, 1);
}

static void put_cell_char(int cx, int cy, char ch)
{
    gfx_blit_char(cx * GLYPH_W, cy * GLYPH_H, ch, fg_idx_cur, 1);
}

void gfx_move_cursor(void)
{
    int x = cur_x * GLYPH_W, y = cur_y * GLYPH_H + GLYPH_H - 2;
    for (int i = 0; i < GLYPH_W; i++) { px(x + i, y, fg_col); px(x + i, y + 1, fg_col); }
}

/* erase only the 2-pixel-high cursor underline under (cur_x, cur_y).
 * Does NOT touch any glyph pixels above, so cells with existing text are
 * preserved. Called BEFORE updating cur_x/cur_y so the old cursor line is
 * wiped before we draw a character or jump to a new cursor position. */
static void hide_cursor(void)
{
    int x = cur_x * GLYPH_W, y = cur_y * GLYPH_H + GLYPH_H - 2;
    for (int i = 0; i < GLYPH_W; i++) { px(x + i, y, bg_col); px(x + i, y + 1, bg_col); }
}

/* the cell under the cursor is always the next-to-write cell, i.e. blank,
 * so wiping it with the background color fully removes the cursor */
static void erase_cursor(void)
{
    int x0 = cur_x * GLYPH_W, y0 = cur_y * GLYPH_H;
    for (int y = 0; y < GLYPH_H; y++)
        for (int x = 0; x < GLYPH_W; x++)
            px(x0 + x, y0 + y, bg_col);
}

static void scroll(void)
{
    /* move everything up one row of pixels*GLYPH_H */
    uint32_t row_words = SCR_W * GLYPH_H;
    for (uint32_t i = 0; i < (uint32_t)SCR_W * (SCR_H - GLYPH_H); i++)
        fb[i] = fb[i + row_words];
    for (int y = SCR_H - GLYPH_H; y < SCR_H; y++)
        for (int x = 0; x < SCR_W; x++)
            px(x, y, bg_col);
}

void gfx_putc(char c)
{
    if (!gfx_on) return;
    hide_cursor();                              /* FIRST: wipe the 2-px cursor
                                                 * line at the OLD position so
                                                 * it won't ghost next to the
                                                 * new cursor (looks "stretched"
                                                 * especially on backspace). */
    if (c == '\n') {
        erase_cursor();                         /* full cell wipe for the (blank)
                                                 * cell that used to hold the
                                                 * cursor — newline writes no
                                                 * glyph here. */
        sb_clear_tail(cur_y, cur_x);            /* mirror row is complete */
        cur_x = 0; cur_y++;
    } else if (c == '\r') {
        cur_x = 0;
    } else if (c == '\b') {
        if (cur_x > 0) cur_x--;
        draw_cell(cur_x, cur_y);                /* erase glyph + refresh bg */
        sb_capture(cur_x, cur_y, ' ');
    } else if (c == '\v') {                     /* cursor left, keep the glyph */
        if (cur_x > 0) cur_x--;
    } else {
        put_cell_char(cur_x, cur_y, c);
        sb_capture(cur_x, cur_y, c);
        cur_x++;
        if (cur_x >= con_cols) { cur_x = 0; cur_y++; }
    }
    if (cur_y >= con_rows) { scroll(); sb_push_top(); cur_y = con_rows - 1; }
    gfx_move_cursor();
}

void gfx_puts(const char *s)
{
    while (*s) gfx_putc(*s++);
}

void gfx_set_colors(uint8_t fg_idx, uint8_t bg_idx)
{
    fg_idx_cur = fg_idx & 0x0F;
    bg_idx_cur = bg_idx & 0x0F;
    fg_col = pal[fg_idx_cur];
    bg_col = pal[bg_idx_cur];
}

void gfx_set_bg_rgb(uint32_t rgb)
{
    bg_col = rgb;
}

void gfx_clear(void)
{
    uint32_t n = (uint32_t)SCR_W * SCR_H;
    for (uint32_t i = 0; i < n; i++) fb[i] = bg_col;
    for (int r = 0; r < 48; r++)
        for (int c = 0; c < SB_COLS; c++) sb_view[r][c] = SB_PACK(fg_idx_cur, bg_idx_cur, ' ');
    cur_x = cur_y = 0;
    sb_off = 0;
    gfx_move_cursor();
}

void gfx_grad_bar(void)
{
    int y0 = cur_y * GLYPH_H;
    for (int x = 0; x < SCR_W; x++) {
        /* dark blue -> cyan -> white -> cyan -> dark blue */
        int t = x * 512 / SCR_W;                       /* 0..511 */
        int d = t < 256 ? t : 511 - t;                 /* 0..255 triangle */
        uint32_t r = (uint32_t)(d * 0x60 / 255) + (d > 200 ? (uint32_t)(d - 200) * 8 : 0);
        uint32_t g = (uint32_t)(40 + d * 215 / 255);
        uint32_t b = (uint32_t)(80 + d * 175 / 255);
        uint32_t c = (r << 16) | (g << 8) | b;
        for (int y = 0; y < GLYPH_H; y++) px(x, y0 + y, c);
    }
    cur_y++;
    cur_x = 0;
}

int gfx_init(void)
{
    capture_font();                 /* must happen while still in text mode */

    /* sanity: reject an empty font AND an open-bus read (all 0xFF) */
    uint32_t sum = 0, ones = 0;
    for (int r = 0; r < GLYPH_H; r++) {
        sum += font['A'][r];
        ones += (font['A'][r] == 0xFF) + (font['a'][r] == 0xFF) + (font['.'][r] == 0xFF);
    }
    if (sum == 0 || ones == GLYPH_H * 3) { fail_stage = 1; return 0; }

    uint32_t lfb = find_vga_lfb();
    dbg_bar = lfb;
    if (!lfb) { fail_stage = 2; return 0; }
    if (vbe_set_mode(SCR_W, SCR_H, SCR_BPP) < 0) { fail_stage = 3; return 0; }

    fb = (volatile uint32_t *)lfb;
    lfb_base = lfb;
    con_cols = SCR_W / GLYPH_W;     /* 128 */
    con_rows = SCR_H / GLYPH_H;     /* 48 */
    cur_x = cur_y = 0;
    gfx_on = 1;
    gfx_set_colors(7, 0);
    gfx_clear();
    return 1;
}
