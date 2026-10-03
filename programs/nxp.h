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
typedef unsigned char u8;

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
    /* accumulated deltas since the last call (screen convention:
     * +x right, +y down). btns: bit0=L bit1=R bit2=M. wheel: notches
     * since last call, +1 = up, -1 = down (always 0 without a wheel).
     * Returns the number of packets consumed (0 = no movement). */
    int  (*mouse)(int *dx, int *dy, int *btns, int *wheel);
    u32 (*get_pixel)(u32 x, u32 y);     /* read a pixel (software cursors) */
    void (*cls)(void);                   /* clear screen + home cursor       */
    void (*set_color)(u32 fg);           /* VGA attr foreground (see below)   */
    int  (*getuser)(char *buf, u32 max); /* copy current login name into buf  */
    int  (*getdate)(char *buf, u32 max); /* formatted RTC date line into buf  */
    int  (*readfile)(const char *name, unsigned char *buf, u32 max);
                                         /* read a NovaFS file (current dir),
                                          * returns size, -1 = error          */
    int  (*spawn)(const char *name);     /* run an .nxp as a new process;
                                          * name.1/2/3.nxp picks the slot,
                                          * returns pid (1..4), -1 = error    */
    int  (*procs)(char *buf, u32 max);   /* "pid name\n" lines of the live
                                          * processes into buf, returns count */
    int  (*writefile)(const char *name, unsigned char *data, u32 len);
                                         /* create/overwrite a NovaFS file
                                          * (owner checks apply), returns
                                          * len or -1                        */
    int  (*listdir)(char *buf, u32 max); /* ls of the cwd into buf, entries  */
    int  (*fsop)(u32 op, const char *name);
                                         /* 1=mkdir 2=rmdir 3=rd 4=rm 5=cd
                                          * 6=format 7=fsinfo (prints)       */
    int  (*sysop)(u32 op, const char *name);
                                         /* 1=useradd 2=userdel 3=passwd
                                          * 4=su 5=mkdemo 6=acpi 7=reboot
                                          * 8=shutdown 9=halt 10=fg 11=kill  */
    u32  (*ticks)(void);                 /* 10 ms since boot (game timing)   */
    /* ---- cell-addressed TUI primitives (VGA attr: fg | bg<<4) ----   */
    void (*putcell)(u32 x, u32 y, u32 ch, u32 attr);
                                         /* write one character cell         */
    void (*cellfill)(u32 x, u32 y, u32 w, u32 h, u32 chattr);
                                         /* fill a rect: chattr = ch<<8|attr */
    void (*cputs)(u32 x, u32 y, const char *s, u32 attr);
                                         /* print text at cell coords        */
    void (*cursor)(u32 on);              /* hide/show the text cursor        */
} nxp_api_t;

/* set_color() foreground values (VGA attribute low nibble) */
#define NXP_COLOR_BLACK    0x00
#define NXP_COLOR_BLUE     0x01
#define NXP_COLOR_GREEN    0x02
#define NXP_COLOR_CYAN     0x03
#define NXP_COLOR_RED      0x04
#define NXP_COLOR_MAGENTA  0x05
#define NXP_COLOR_BROWN    0x06
#define NXP_COLOR_LGRAY    0x07
#define NXP_COLOR_DGRAY    0x08
#define NXP_COLOR_LBLUE   0x09
#define NXP_COLOR_LGREEN  0x0A
#define NXP_COLOR_LCYAN   0x0B
#define NXP_COLOR_LRED    0x0C
#define NXP_COLOR_LMAGENTA 0x0D
#define NXP_COLOR_YELLOW  0x0E
#define NXP_COLOR_WHITE   0x0F

#define NXP_BTN_L 1
#define NXP_BTN_R 2
#define NXP_BTN_M 4

/* extended key codes (arrows) returned by getchar()/getkey() */
#define NXP_KEY_LEFT  0x11
#define NXP_KEY_RIGHT 0x12
#define NXP_KEY_UP    0x13
#define NXP_KEY_DOWN  0x14

/* TUI helpers: VGA attribute byte = fg | bg<<4 (16-color palette) */
#define NXP_ATTR(fg, bg)  (((bg) << 4) | (fg))

/* CP437 box-drawing glyphs (present in the captured VGA 8x16 font) */
#define NXP_CH_H     0xCD   /* ═ */
#define NXP_CH_V     0xBA   /* ║ */
#define NXP_CH_TL    0xC9   /* ╔ */
#define NXP_CH_TR    0xBB   /* ╗ */
#define NXP_CH_BL    0xC8   /* ╚ */
#define NXP_CH_BR    0xBC   /* ╝ */
#define NXP_CH_TL_S  0xDA   /* ┌ */
#define NXP_CH_TR_S  0xBF   /* ┐ */
#define NXP_CH_BL_S  0xC0   /* └ */
#define NXP_CH_BR_S  0xD9   /* ┘ */
#define NXP_CH_H_S   0xC4   /* ─ */
#define NXP_CH_V_S   0xB3   /* │ */
#define NXP_CH_BLOCK 0xDB   /* █ */
#define NXP_CH_SHADE 0xB0   /* ░ (scrollbar track) */
#define NXP_CH_BULLET 0x07  /* ● */
#define NXP_CH_DOT   0xFA   /* · */
#define NXP_CH_LARROW 0x11
#define NXP_CH_RARROW 0x10
#define NXP_CH_UARROW 0x18
#define NXP_CH_DARROW 0x19

extern nxp_api_t *nxp_api_ptr;   /* set by nxp_entry.c before nxp_main() */
#define API nxp_api_ptr

void nxp_main(void);             /* your program's real entry */

#endif
