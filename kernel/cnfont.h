/* ============================================================
 * NovaOS - tiny CJK glyph bank (cn_lang_support, phase 1)
 * ============================================================ */
#ifndef CNFONT_H
#define CNFONT_H

#include "stdint.h"

/* 16x16 glyph bitmap for a Unicode codepoint, 0 = not in bank */
const uint8_t *cn_glyph(unsigned short code);

/* 1 = every char of the UTF-8 string is ASCII or in the bank */
int cn_can_render(const char *s);

/* render a UTF-8 string with 8x16 ASCII + 16x16 CJK glyphs at pixel
 * coords; returns the advance width in pixels, -1 = a char is missing
 * from the bank (caller must cn_can_render() first or fall back) */
int cn_text16(int x, int y, const char *s, uint32_t rgb);

#endif /* CNFONT_H */
