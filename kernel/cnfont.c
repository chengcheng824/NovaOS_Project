/* ============================================================
 * NovaOS - tiny CJK glyph bank (cn_lang_support, phase 1)
 *
 * Hand-drawn 16x16 bitmaps for exactly the characters the National
 * Day egg needs. Full CJK fonts are hundreds of KB - impossible in
 * this kernel - so cn_lang_support ships a minimal bank: a string
 * is renderable only if every char is in the bank (cn_can_render).
 *
 * Encoding: each glyph is 16 rows x 2 bytes, row-major, top bit =
 * leftmost pixel. Index = codepoint - 0xB0A1 region trick avoided;
 * we use a compact lookup of (codepoint, bitmap) pairs.
 * ============================================================ */
#include "cnfont.h"
#include "stdint.h"

typedef struct {
    unsigned short code;    /* Unicode BMP codepoint (GBK 0xB0A1-range chars) */
    uint8_t bits[32];       /* 16 rows x 2 bytes */
} glyph_t;

/* Real simplified glyphs rendered from the host font by tools/genfont.ps1 */
#include "cnfont_glyphs.h"

#define BANK_N (sizeof bank / sizeof bank[0])

const uint8_t *cn_glyph(unsigned short code)
{
    for (unsigned i = 0; i < BANK_N; i++)
        if (bank[i].code == code) return bank[i].bits;
    return 0;
}

/* every char of a UTF-8 string must be in the bank (ASCII ok too) */
int cn_can_render(const char *s)
{
    while (*s) {
        unsigned char c = (unsigned char)*s;
        if (c < 0x80) { s++; continue; }                    /* ASCII */
        if ((c & 0xE0) == 0xC0 && (s[1] & 0xC0) == 0x80) {
            unsigned short cp = ((unsigned short)(c & 0x1F) << 6)
                              | (unsigned short)(s[1] & 0x3F);
            if (!cn_glyph(cp)) return 0;
            s += 2;
            continue;
        }
        if ((c & 0xF0) == 0xE0 && (s[1] & 0xC0) == 0x80 && (s[2] & 0xC0) == 0x80) {
            unsigned short cp = ((unsigned short)(c & 0x0F) << 12)
                              | ((unsigned short)(s[1] & 0x3F) << 6)
                              | (unsigned short)(s[2] & 0x3F);
            if (!cn_glyph(cp)) return 0;
            s += 3;
            continue;
        }
        return 0;                                           /* bad byte */
    }
    return 1;
}
