/* paint.nxp - interactive doodle pad.
 * Left-drag = paint, right-drag = erase, mouse alone = move the cursor.
 * Arrows/WASD also draw, 1-8 pick color, c clears, q quits. */
#include "nxp.h"

static const u32 cols[8] = {
    0x00FFFFFF, 0x00FF5555, 0x0055FF55, 0x005555FF,
    0x00FFFF55, 0x00FF55FF, 0x0055FFFF, 0x00FF8000
};

static const char *names[8] = {
    "white", "red", "green", "blue", "yellow", "magenta", "cyan", "orange"
};

static void toolbar(void)
{
    API->fill_rect(0, 0, API->scr_w, 40, 0x00101820);
    API->text(8, 12, "paint: L-draw  R-erase  1-8 color  c clear  q quit", 0x00FFFFFF);
    for (int i = 0; i < 8; i++)
        API->fill_rect(520 + i * 32, 14, 24, 12, cols[i]);
}

/* draw a thick line by stamping filled rects along the path */
static void stroke(int x0, int y0, int x1, int y1, u32 rgb, int sz)
{
    int adx = x1 - x0, ady = y1 - y0;
    if (adx < 0) adx = -adx;
    if (ady < 0) ady = -ady;
    int n = adx > ady ? adx : ady;
    if (n == 0) n = 1;
    int h = sz / 2;
    int dx = x1 - x0, dy = y1 - y0;
    for (int i = 0; i <= n; i++) {
        int px = x0 + dx * i / n;
        int py = y0 + dy * i / n;
        int rx = px - h, ry = py - h;
        if (ry < 41) ry = 41;
        if (rx < 0) rx = 0;
        API->fill_rect(rx, ry, sz, sz, rgb);
    }
}

/* XOR crosshair cursor: calling it twice restores the original pixels,
 * so it is visible on any background and leaves no trail. */
static void cursor_xor(int cx, int cy)
{
    int w = (int)API->scr_w, h = (int)API->scr_h;
    for (int d = -5; d <= 5; d++) {
        if (!d) continue;
        int x = cx + d, y = cy + d;
        if (x >= 0 && x < w && cy >= 0 && cy < h)
            API->pixel(x, cy, API->get_pixel(x, cy) ^ 0x00FFFFFFu);
        if (cx >= 0 && cx < w && y >= 0 && y < h)
            API->pixel(cx, y, API->get_pixel(cx, y) ^ 0x00FFFFFFu);
    }
    if (cx >= 0 && cx < w && cy >= 0 && cy < h)
        API->pixel(cx, cy, API->get_pixel(cx, cy) ^ 0x00FFFFFFu);
}

void nxp_main(void)
{
    if (!API->scr_w) {
        API->puts("paint.nxp needs the graphics console\n");
        API->exit();
    }

    int col = 0;
    int x = (int)API->scr_w / 2;
    int y = (int)API->scr_h / 2;
    int shown = 0;      /* cursor currently XORed onto the canvas? */

    API->fill_rect(0, 0, API->scr_w, API->scr_h, 0x00000000);
    toolbar();

    for (;;) {
        /* ---- mouse ---- */
        int mdx, mdy, mb, mw;
        if (API->mouse(&mdx, &mdy, &mb, &mw)) {
            if (shown) { cursor_xor(x, y); shown = 0; }  /* lift at old pos */
            int px0 = x, py0 = y;
            x += mdx; y += mdy;
            if (mb & NXP_BTN_L)
                stroke(px0, py0, x, y, cols[col], 4);
            else if (mb & NXP_BTN_R)
                stroke(px0, py0, x, y, 0x00000000, 8);
        }

        /* ---- keyboard ---- */
        int k = API->getkey();
        if (k >= 0) {
            if (k == 'q' || k == 27) {
                if (shown) cursor_xor(x, y);
                break;
            }
            if (k == 'c') {
                if (shown) { cursor_xor(x, y); shown = 0; }
                API->fill_rect(0, 0, API->scr_w, API->scr_h, 0x00000000);
                toolbar();
            } else if (k >= '1' && k <= '8') {
                col = k - '1';
                if (shown) { cursor_xor(x, y); shown = 0; }
                API->text(440, 12, "        ", 0);
                API->text(440, 12, names[col], 0x00AAAAAA);
            } else {
                int mx = 0, my = 0;
                if (k == NXP_KEY_LEFT  || k == 'a') mx = -4;
                if (k == NXP_KEY_RIGHT || k == 'd') mx = 4;
                if (k == NXP_KEY_UP    || k == 'w') my = -4;
                if (k == NXP_KEY_DOWN  || k == 's') my = 4;
                if (mx || my) {
                    if (shown) { cursor_xor(x, y); shown = 0; }
                    x += mx; y += my;
                    if (x > 1 && y > 41)
                        API->fill_rect(x - 2, y - 2, 4, 4, cols[col]);
                }
            }
        }

        if (!shown) { cursor_xor(x, y); shown = 1; }     /* rest at new pos */
    }

    API->exit();
}
