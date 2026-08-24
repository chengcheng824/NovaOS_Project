/* ============================================================
 * nxp.h - .nxp program API (kernel-provided, cdecl)
 *
 * Program layout: linked at 0x00300004 (after the 4-byte
 * "NXP\x01" magic), entry = first byte, EAX = nxp_api_t*.
 * Link with nxp_entry.o FIRST so the entry stub is at offset 0.
 * ============================================================ */
#ifndef NXP_H
#define NXP_H

typedef unsigned int u32;

typedef struct {
    u32 magic;                          /* 'NXP1' */
    u32 version;                        /* 1 */
    void (*putc)(char);
    void (*puts)(const char *);
    int  (*getchar)(void);              /* blocking key */
    void (*exit)(void);                 /* return to shell */
    u32 scr_w, scr_h;                   /* 0,0 = graphics unavailable */
    void (*pixel)(u32 x, u32 y, u32 rgb);
    void (*fill_rect)(u32 x, u32 y, u32 w, u32 h, u32 rgb);
    void (*text)(u32 x, u32 y, const char *s, u32 rgb);   /* 8x16 font */
    int  (*getkey)(void);          /* non-blocking: -1 = no key */
    /* accumulated mouse deltas since the last call (screen convention:
     * +x right, +y down). btns: bit0=L bit1=R bit2=M. Returns the number
     * of packets consumed (0 = no movement). */
    int  (*mouse)(int *dx, int *dy, int *btns);
    u32 (*get_pixel)(u32 x, u32 y);     /* read a pixel (software cursors) */
} nxp_api_t;

#define NXP_BTN_L 1
#define NXP_BTN_R 2
#define NXP_BTN_M 4

/* extended key codes (arrows) returned by getchar()/getkey() */
#define NXP_KEY_LEFT  0x11
#define NXP_KEY_RIGHT 0x12
#define NXP_KEY_UP    0x13
#define NXP_KEY_DOWN  0x14

extern nxp_api_t *nxp_api_ptr;   /* set by nxp_entry.c before nxp_main() */
#define API nxp_api_ptr

void nxp_main(void);             /* your program's real entry */

#endif
