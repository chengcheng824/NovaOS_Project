/* ============================================================
 * featureui.nxp - browse/toggle feature switches (Ring 3)
 *
 * A compact TUI over sysop 19 (feature list) / 20 (feature set):
 * up/down picks a module, Space or Left/Right toggles it in place,
 * 'w' re-reads the list (as the kernel stored it), q quits.
 * State comes straight from /etc/features.conf via the kernel.
 * ============================================================ */
#include "nxp.h"

/* theme (match tui.nxp's dark look) */
#define A_RULE   NXP_ATTR(NXP_COLOR_DGRAY,  NXP_COLOR_BLACK)
#define A_TITLE  NXP_ATTR(NXP_COLOR_LCYAN,  NXP_COLOR_BLACK)
#define A_TXT    NXP_ATTR(NXP_COLOR_LGRAY,  NXP_COLOR_BLACK)
#define A_DIM    NXP_ATTR(NXP_COLOR_DGRAY,  NXP_COLOR_BLACK)
#define A_SEL    NXP_ATTR(NXP_COLOR_WHITE,  NXP_COLOR_DGRAY)
#define A_ON     NXP_ATTR(NXP_COLOR_LGREEN, NXP_COLOR_BLACK)
#define A_OFF    NXP_ATTR(NXP_COLOR_LRED,   NXP_COLOR_BLACK)
#define A_ONSEL  NXP_ATTR(NXP_COLOR_LGREEN, NXP_COLOR_DGRAY)
#define A_OFFSEL NXP_ATTR(NXP_COLOR_LRED,   NXP_COLOR_DGRAY)

#define MAXF 16
static char f_name[MAXF][24];
static char f_state[MAXF][10];
static int  f_n, f_sel;
static int  COLS, ROWS;
static char lastmsg[48];

static int slen(const char *s) { int n = 0; while (s[n]) n++; return n; }

static void msg(const char *m)
{
    int i = 0;
    while (m[i] && i < 47) { lastmsg[i] = m[i]; i++; }
    lastmsg[i] = 0;
}

static void load_list(void)
{
    char lb[512];                               /* stack: syscall buffer */
    f_n = API->sysop(19, lb);                   /* returns count */
    if (f_n < 0) f_n = 0;
    if (f_n > MAXF) f_n = MAXF;
    const char *p = lb;
    for (int i = 0; i < f_n; i++) {
        int k = 0;
        while (*p && *p != ' ') { if (k < 23) f_name[i][k++] = *p++; }
        f_name[i][k] = 0;
        while (*p == ' ') p++;                  /* skip column padding */
        k = 0;
        while (*p && *p != '\n') { if (k < 9) f_state[i][k++] = *p++; }
        f_state[i][k] = 0;
        if (*p) p++;
    }
}

static void set_state(int i, const char *onoff)
{
    char arg[36];                               /* stack: syscall buffer */
    int k = 0;
    for (int j = 0; f_name[i][j] && k < 23; j++) arg[k++] = f_name[i][j];
    arg[k++] = '=';
    for (int j = 0; onoff[j]; j++) arg[k++] = onoff[j];
    arg[k] = 0;
    int r = API->sysop(20, arg);
    if (r == 0) {
        int j = 0;
        while (onoff[j] && j < 9) { f_state[i][j] = onoff[j]; j++; }
        f_state[i][j] = 0;
        msg("saved to /etc/features.conf");
    } else {
        msg("kernel refused the change");
    }
}

static void draw(void)
{
    API->cellfill(0, 0, COLS, ROWS, ((u32)' ' << 8) | A_TXT);
    /* header */
    API->cellfill(0, 0, COLS, 1, ((u32)' ' << 8) | A_TXT);
    API->cputs(2, 0, "Feature Manager", A_TITLE);
    API->cputs(COLS - 20, 0, "/etc/features.conf", A_DIM);
    API->cellfill(0, 1, COLS, 1, ((u32)NXP_CH_H_S << 8) | A_RULE);

    if (!f_n)
        API->cputs(4, 3, "no features registered", A_DIM);
    for (int i = 0; i < f_n; i++) {
        int y = 3 + i * 2;
        int sel = (i == f_sel);
        int on = (f_state[i][0] == 'e');
        API->cellfill(2, y, COLS - 4, 1,
                      ((u32)' ' << 8) | (sel ? A_SEL : A_TXT));
        API->putcell(2, y, sel ? NXP_CH_RARROW : NXP_CH_DOT,
                     sel ? A_SEL : A_DIM);
        API->cputs(4, y, f_name[i], sel ? A_SEL : A_TXT);
        char pill[14];
        int k = 0;
        pill[k++] = '[';
        for (int j = 0; f_state[i][j] && k < 12; j++) pill[k++] = f_state[i][j];
        pill[k++] = ']'; pill[k] = 0;
        API->cputs(30, y, pill,
                   sel ? (on ? A_ONSEL : A_OFFSEL) : (on ? A_ON : A_OFF));
    }

    /* status + hint bar */
    API->cellfill(0, ROWS - 2, COLS, 1, ((u32)' ' << 8) | A_TXT);
    API->cputs(2, ROWS - 2, lastmsg, A_DIM);
    API->cellfill(0, ROWS - 1, COLS, 1, ((u32)' ' << 8) | A_TXT);
    API->cputs(2, ROWS - 1,
               "up/down pick   space toggle   r reload   q quit", A_DIM);
}

void nxp_main(void)
{
    COLS = API->scr_w ? (int)(API->scr_w / 8) : 80;
    ROWS = API->scr_h ? (int)(API->scr_h / 16) : 25;
    f_sel = 0;
    lastmsg[0] = 0;

    API->cursor(0);
    load_list();
    msg("read /etc/features.conf");
    draw();

    for (;;) {
        int c = API->getkey();
        if (c < 0) continue;
        if (c == 'q' || c == 'Q' || c == 27) break;
        else if (c == NXP_KEY_UP && f_sel > 0) { f_sel--; draw(); }
        else if (c == NXP_KEY_DOWN && f_sel < f_n - 1) { f_sel++; draw(); }
        else if (c == ' ' || c == NXP_KEY_LEFT || c == NXP_KEY_RIGHT) {
            if (f_n) {
                int on = (f_state[f_sel][0] == 'e');
                set_state(f_sel, on ? "disable" : "enable");
                draw();
            }
        }
        else if (c == 'r' || c == 'R') { load_list(); msg("reloaded"); draw(); }
    }
    API->cursor(1);
    API->exit();
}
