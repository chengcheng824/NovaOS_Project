/* ============================================================
 * NovaOS - minimal graphics driver: Bochs VBE + LFB console
 *
 *   1. capture the VGA BIOS 8x16 font from plane 2 (text mode only!)
 *   2. scan PCI for a display controller (class 03), read BAR0 = LFB
 *   3. program the Bochs VBE registers (0x1CE/0x1CF) from protected
 *      mode - no BIOS calls needed: 1024x768x32
 *   4. render a 128x48 true-color text console into the linear
 *      framebuffer (put pixel / char / scroll / cursor)
 *   If any step fails, gfx_init() returns 0 and the caller keeps
 *   the classic 80x25 VGA text console.
 * ============================================================ */
#ifndef GFX_H
#define GFX_H

#include "stdint.h"

int  gfx_init(void);       /* 1 = LFB console active, 0 = fallback */
int  gfx_active(void);
int  gfx_fail_stage(void); /* 0 ok; 1 font, 2 pci, 3 vbe */
uint16_t gfx_dbg_id(void);
uint32_t gfx_dbg_bar(void);
uint32_t gfx_lfb(void);    /* PCI BAR of the linear framebuffer (0 = graphics off) */
uint32_t gfx_dbg_state(void);   /* debug: on<<24 | cur_y<<12 | cur_x */
const void *gfx_dbg_fb(void);   /* debug: LFB pointer */
const uint8_t *gfx_dbg_font(int c);
int  gfx_cols(void);
int  gfx_rows(void);

void gfx_putc(char c);
void gfx_puts(const char *s);
void gfx_clear(void);
void gfx_set_colors(uint8_t fg_idx, uint8_t bg_idx);   /* VGA 16-color idx */
void gfx_set_bg_rgb(uint32_t rgb);  /* set raw RGB background (for BSOD) */
void gfx_move_cursor(void);
/* scrollback view (gfx console only): delta > 0 views older lines,
 * < 0 newer, 0 returns to the live view. ~100 lines of history. */
void gfx_sb_scroll(int delta);
void gfx_grad_bar(void);   /* true-color gradient bar, one text row tall */
/* draw one font glyph at pixel coords with integer scale */
void gfx_blit_char(int px, int py, char ch, uint8_t fg_idx, int scale);

/* raw primitives for user programs (.nxp API) */
int  gfx_ready(void);
void gfx_pixel(int x, int y, uint32_t rgb);
uint32_t gfx_pixel_get(int x, int y);   /* for software cursors (XOR sprites) */
void gfx_text(int x, int y, const char *s, uint32_t rgb);
/* cell-addressed glyph with explicit colors (TUI); no cursor/scroll side
 * effects. VGA attr convention: fg = attr & 0xF, bg = (attr >> 4) & 0xF */
void gfx_cell(int cx, int cy, char ch, uint8_t fg_idx, uint8_t bg_idx);

/* CJK 16x16 text (cn_lang_support): UTF-8 in, mixed 8/16px advance out;
 * -1 = a char is outside the glyph bank */
int cn_text16(int x, int y, const char *s, uint32_t rgb);

#endif
