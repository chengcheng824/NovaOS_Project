/* snake.nxp - the first NovaOS game.
 *
 * Arrows/WASD steer the snake, red blocks are food, every bite speeds
 * the game up. Hitting a wall or yourself ends the run; SPACE restarts,
 * ESC/q returns to the shell.
 *
 * Movement is stepped by API->ticks() (the kernel's 10 ms IRQ0 counter),
 * so the speed is identical on any host CPU. Incremental drawing: only
 * the new head, the dimmed old head, the food and the vacated tail are
 * ever repainted. */
#include "nxp.h"

#define CELL   24
#define COLS   40
#define ROWS   28
#define OX     ((1024 - COLS * CELL) / 2)      /* 32  */
#define OY     56                              /* below the score bar */
#define MAXLEN (COLS * ROWS)

#define C_FOOD 0x00FF5555u
#define C_BODY 0x0055FF55u
#define C_HEAD 0x00AAFFAAu
#define C_BG   0x00000000u
#define C_BAR  0x00101820u
#define C_EDGE 0x00404040u

static unsigned char sx[MAXLEN], sy[MAXLEN];
static int  slen, dx, dy, foodx, foody, dead;   /* dead: 1 = crash, 2 = win */
static u32  score, best, rng;
static int  over_shown;

static u32 rnd(void) { rng = rng * 1103515245u + 12345u; return rng >> 8; }

static void utoa10(char *dst, u32 v)
{
    char t[12]; int k = 0, i = 0;
    if (!v) t[k++] = '0';
    while (v) { t[k++] = (char)('0' + v % 10); v /= 10; }
    while (k--) dst[i++] = t[k];
    dst[i] = 0;
}

static void cell_rect(int cx, int cy, u32 rgb)
{
    API->fill_rect(OX + cx * CELL + 1, OY + cy * CELL + 1, CELL - 2, CELL - 2, rgb);
}

static void draw_edges(void)
{
    API->fill_rect(OX - 3, OY - 3, COLS * CELL + 6, 3, C_EDGE);
    API->fill_rect(OX - 3, OY + ROWS * CELL, COLS * CELL + 6, 3, C_EDGE);
    API->fill_rect(OX - 3, OY - 3, 3, ROWS * CELL + 6, C_EDGE);
    API->fill_rect(OX + COLS * CELL, OY - 3, 3, ROWS * CELL + 6, C_EDGE);
}

static void draw_bar(void)
{
    char b[12];
    API->fill_rect(0, 0, API->scr_w, 40, C_BAR);
    API->text(8, 12, "snake: arrows/WASD   q quit", 0x00FFFFFF);
    API->text(600, 12, "score", 0x00AAAAAA);
    utoa10(b, score); API->text(660, 12, b, 0x00FFFFFF);
    API->text(760, 12, "best", 0x00AAAAAA);
    utoa10(b, best);  API->text(810, 12, b, 0x00FFFFFF);
}

static void place_food(void)
{
    for (;;) {
        int fx = (int)(rnd() % COLS);
        int fy = (int)(rnd() % ROWS);
        int hit = 0;
        for (int i = 0; i < slen; i++)
            if (sx[i] == fx && sy[i] == fy) { hit = 1; break; }
        if (!hit) {
            foodx = fx; foody = fy;
            cell_rect(fx, fy, C_FOOD);
            return;
        }
    }
}

static void reset(void)
{
    API->fill_rect(0, 0, API->scr_w, API->scr_h, C_BG);
    draw_edges();
    draw_bar();
    slen = 4; dx = 1; dy = 0; score = 0; dead = 0; over_shown = 0;
    for (int i = 0; i < slen; i++) { sx[i] = (unsigned char)(8 - i); sy[i] = 12; }
    for (int i = 0; i < slen; i++) cell_rect(sx[i], sy[i], C_BODY);
    rng = API->ticks() | 1;
    place_food();
}

static void step(void)
{
    int nx = (int)sx[0] + dx, ny = (int)sy[0] + dy;
    if (nx < 0 || ny < 0 || nx >= COLS || ny >= ROWS) { dead = 1; return; }

    int eating = (nx == foodx && ny == foody);
    int lim = eating ? slen : slen - 1;          /* the tail cell vacates */
    for (int i = 0; i < lim; i++)
        if (sx[i] == nx && sy[i] == ny) { dead = 1; return; }

    if (eating) slen++;
    for (int i = slen - 1; i > 0; i--) { sx[i] = sx[i - 1]; sy[i] = sy[i - 1]; }
    sx[0] = (unsigned char)nx; sy[0] = (unsigned char)ny;

    cell_rect(nx, ny, C_HEAD);                   /* bright new head   */
    if (slen > 1) cell_rect(sx[1], sy[1], C_BODY);  /* dim the old head */
    if (eating) {
        score += 10;
        draw_bar();
        if (slen >= MAXLEN) { dead = 2; return; }    /* filled the board */
        place_food();
    } else {
        cell_rect(sx[slen - 1], sy[slen - 1], C_BG); /* vacated tail */
    }
}

void nxp_main(void)
{
    if (!API->scr_w) {
        API->puts("snake.nxp needs the graphics console\n");
        API->exit();
    }

    u32 last = API->ticks();
    int delay = 12;                              /* ticks per step (120 ms) */
    reset();

    for (;;) {
        int k = API->getkey();
        if (k >= 0) {
            if (k == 'q' || k == 27) API->exit();
            if (dead) {
                if (k == ' ') { reset(); delay = 12; last = API->ticks(); }
            } else {
                int ndx = dx, ndy = dy;
                if (k == NXP_KEY_LEFT  || k == 'a') { ndx = -1; ndy = 0; }
                if (k == NXP_KEY_RIGHT || k == 'd') { ndx =  1; ndy = 0; }
                if (k == NXP_KEY_UP    || k == 'w') { ndx = 0; ndy = -1; }
                if (k == NXP_KEY_DOWN  || k == 's') { ndx = 0; ndy =  1; }
                /* no instant reversal, no no-op */
                if ((ndx != dx || ndy != dy) && !(ndx == -dx && ndy == -dy)) {
                    dx = ndx; dy = ndy;
                }
            }
        }

        if (dead) {
            if (!over_shown) {
                over_shown = 1;
                if (score > best) best = score;
                draw_bar();
                char b[12];
                API->fill_rect(262, 300, 500, 150, 0x00202030);
                API->text(370, 320, dead == 2 ? "YOU WIN!" : "GAME OVER",
                          0x00FF5555);
                API->text(300, 366, "score:", 0x00AAAAAA);
                utoa10(b, score); API->text(380, 366, b, 0x00FFFFFF);
                API->text(470, 366, "best:", 0x00AAAAAA);
                utoa10(b, best);  API->text(530, 366, b, 0x00FFFFFF);
                API->text(310, 400, "SPACE = again    ESC/q = quit",
                          0x00AAAAAA);
            }
            continue;
        }

        u32 now = API->ticks();
        if (now - last >= (u32)delay) {
            last = now;
            step();
            delay = 12 - (int)(score / 50);      /* faster with every bite */
            if (delay < 5) delay = 5;
        }
    }
}
