/* ============================================================
 * NovaOS - graphics driver implementation (see gfx.h)
 * ============================================================ */
#include "stdint.h"
#include "gfx.h"

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

/* cell-addressed glyph with explicit per-cell colors (TUI primitives).
 * Bypasses the text console entirely: no cursor, no scrolling. */
void gfx_cell(int cx, int cy, char ch, uint8_t fg_idx, uint8_t bg_idx)
{
    if (!gfx_on || cx < 0 || cy < 0 || cx >= con_cols || cy >= con_rows) return;
    int x0 = cx * GLYPH_W, y0 = cy * GLYPH_H;
    const uint8_t *g = font[(uint8_t)ch];
    uint32_t fg = pal[fg_idx & 0x0F], bg = pal[bg_idx & 0x0F];
    for (int r = 0; r < GLYPH_H; r++) {
        uint8_t bits = g[r];
        for (int b = 0; b < GLYPH_W; b++)
            px(x0 + b, y0 + r, (bits & (0x80 >> b)) ? fg : bg);
    }
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
        cur_x = 0; cur_y++;
    } else if (c == '\r') {
        cur_x = 0;
    } else if (c == '\b') {
        if (cur_x > 0) cur_x--;
        draw_cell(cur_x, cur_y);                /* erase glyph + refresh bg */
    } else if (c == '\v') {                     /* cursor left, keep the glyph */
        if (cur_x > 0) cur_x--;
    } else {
        put_cell_char(cur_x, cur_y, c);
        cur_x++;
        if (cur_x >= con_cols) { cur_x = 0; cur_y++; }
    }
    if (cur_y >= con_rows) { scroll(); cur_y = con_rows - 1; }
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
    cur_x = cur_y = 0;
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
