/* ============================================================
 * tui.nxp - NovaOS fullscreen TUI (Ring 3)
 *
 * Three panels on top of the cell-addressed console syscalls
 * (putcell / cellfill / cputs - see nxp.h):
 *
 *   [1] Files  - NovaFS browser: arrows to move, Enter opens a
 *                directory or views a file, Backspace goes up
 *   [2] Tasks  - live process list, k kills the selected pid
 *   [3] System - user / date / uptime / screen info
 *
 * Keys: 1/2/3 or Left/Right switch panels, q or Esc quits.
 * Works on the 128x48 VBE console AND the 80x25 VGA fallback
 * (dimensions come from scr_w/scr_h; 0 = assume 80x25).
 *
 * Buffer rule: every buffer handed to a syscall (listdir/procs/
 * readfile/getdate/getuser) lives on the STACK - the kernel only
 * accepts user pointers from the 8KB slot-image window or the
 * 32KB stack window, and the big buffers would not fit the image.
 * ============================================================ */
#include "nxp.h"

/* ---- palette (VGA attribute bytes: fg | bg<<4) ---- */
#define A_DESK   NXP_ATTR(NXP_COLOR_LGRAY,  NXP_COLOR_BLACK)   /* screen bg  */
#define A_FRAME  NXP_ATTR(NXP_COLOR_WHITE,  NXP_COLOR_BLUE)    /* panel edge */
#define A_BODY   NXP_ATTR(NXP_COLOR_LGRAY,  NXP_COLOR_BLUE)    /* panel bg   */
#define A_TITLE  NXP_ATTR(NXP_COLOR_BLACK,  NXP_COLOR_LGRAY)   /* top bar    */
#define A_TABSEL NXP_ATTR(NXP_COLOR_WHITE,  NXP_COLOR_BLUE)    /* active tab */
#define A_SEL    NXP_ATTR(NXP_COLOR_YELLOW, NXP_COLOR_RED)     /* selection  */
#define A_LABEL  NXP_ATTR(NXP_COLOR_LCYAN,  NXP_COLOR_BLUE)
#define A_DIM    NXP_ATTR(NXP_COLOR_DGRAY,  NXP_COLOR_BLUE)
#define A_VIEW   NXP_ATTR(NXP_COLOR_LGRAY,  NXP_COLOR_BLACK)   /* file view  */
#define A_ERR    NXP_ATTR(NXP_COLOR_LRED,   NXP_COLOR_BLUE)

/* ---- layout (computed at start) ---- */
#define VIEW_MAX 3072                   /* max bytes shown in the viewer;
                                         * also keeps every stack frame
                                         * under 4KB (no __chkstk_ms) */
static int COLS, ROWS;
static int PX, PY, PW, PH;          /* panel rect (frame included) */
static int TABS_X;                  /* first tab cell on the top bar */
static int g_tab;

/* ---- files state (never passed to syscalls: may live in BSS) ---- */
#define MAXENT 64
static char f_name[MAXENT][24];
static int  f_dir[MAXENT];
static u32  f_size[MAXENT];
static int  f_n, f_sel, f_top;

/* ---- generic helpers ---- */
static int mini(int a, int b) { return a < b ? a : b; }

/* unsigned -> text (returns new end pointer) */
static char *put_num(char *o, u32 v)
{
    char t[11];
    int m = 0;
    if (!v) t[m++] = '0';
    while (v) { t[m++] = (char)('0' + v % 10); v /= 10; }
    while (m) *o++ = t[--m];
    return o;
}
static char *put_str(char *o, const char *s)
{
    while (*s) *o++ = *s++;
    return o;
}

static void draw_hline(int x, int y, int w, u32 attr, char ch)
{
    API->cellfill(x, y, w, 1, ((u32)(ch & 0xFF) << 8) | attr);
}

/* double-line frame with an embedded title, interior filled */
static void panel(int x, int y, int w, int h, u32 attr, const char *title)
{
    API->cellfill(x, y, w, h, ((u32)' ' << 8) | attr);
    draw_hline(x + 1, y, w - 2, attr, NXP_CH_H);
    draw_hline(x + 1, y + h - 1, w - 2, attr, NXP_CH_H);
    API->cellfill(x, y + 1, 1, h - 2, ((u32)NXP_CH_V << 8) | attr);
    API->cellfill(x + w - 1, y + 1, 1, h - 2, ((u32)NXP_CH_V << 8) | attr);
    API->putcell(x, y, NXP_CH_TL, attr);
    API->putcell(x + w - 1, y, NXP_CH_TR, attr);
    API->putcell(x, y + h - 1, NXP_CH_BL, attr);
    API->putcell(x + w - 1, y + h - 1, NXP_CH_BR, attr);
    if (title && title[0]) API->cputs(x + 2, y, title, attr);
}

/* ========================= top + status bars ========================= */
static const char *tab_name[3] = { " 1 Files ", " 2 Tasks ", " 3 System " };
static const char *keys_hint[3] = {
    "1/2/3 panel   Enter: open   Backspace: up   q: quit",
    "1/2/3 panel   Up/Down: pick   k: kill   q: quit",
    "1/2/3 panel   q: quit",
};

static void draw_bar(void)
{
    char db[64];                                /* stack: syscall buffer */
    API->cellfill(0, 0, COLS, 1, ((u32)' ' << 8) | A_TITLE);
    API->cputs(1, 0, "NovaOS TUI", A_TABSEL);

    TABS_X = COLS / 2 - 16;
    if (TABS_X < 14) TABS_X = 14;
    for (int t = 0; t < 3; t++)
        API->cputs(TABS_X + t * 11, 0, tab_name[t],
                   (t == g_tab) ? A_TABSEL : A_TITLE);

    /* clock on the right edge (getdate line: "... Time: HH:MM:SS") */
    if (API->getdate(db, (u32)sizeof db) > 0) {
        for (int i = 0; db[i]; i++) {
            if (db[i] == 'T' && db[i+1] == 'i' && db[i+2] == 'm' && db[i+3] == 'e') {
                const char *tm = db + i + 6;
                int len = 0;
                while (tm[len] >= ' ' && len < 9) len++;
                while (len && tm[len-1] == ' ') len--;
                if (len) API->cputs(COLS - len - 1, 0, tm, A_TITLE);
                break;
            }
        }
    }
}

static void draw_status(void)
{
    char ub[24];                                /* stack: syscall buffer */
    API->cellfill(0, ROWS - 1, COLS, 1, ((u32)' ' << 8) | A_TITLE);
    API->cputs(1, ROWS - 1, keys_hint[g_tab], A_TITLE);
    if (API->getuser(ub, (u32)sizeof ub) > 0) {
        int len = 0;
        while (ub[len] && len < 14) len++;
        API->cputs(COLS - len - 1, ROWS - 1, ub, A_TITLE);
    }
}

/* =============================== Files =============================== */
static void files_load(void)
{
    char lb[2048];                              /* stack: syscall buffer */
    f_n = 0; f_sel = 0; f_top = 0;
    API->listdir(lb, (u32)sizeof lb);
    const char *p = lb;
    while (*p && f_n < MAXENT) {
        int is_dir;
        if (p[0] == ' ' && p[1] == ' ' && p[2] == '[' && p[3] == 'D')
            is_dir = 1;
        else if (p[0] == ' ' && p[1] == ' ' && p[2] == '[' && p[3] == 'F')
            is_dir = 0;
        else {                                  /* unrelated line: skip */
            while (*p && *p != '\n') p++;
            if (*p) p++;
            continue;
        }

        const char *s = p + 9;                  /* past "  [DIR]  "/"  [FILE] " */
        const char *q = s;
        while (*q && *q != '\n' && *q != '(') q++;
        while (q > s && q[-1] == ' ') q--;      /* pad before "(N B)" */

        int k = 0;
        while (s < q && k < 23) f_name[f_n][k++] = *s++;
        f_name[f_n][k] = 0;
        f_dir[f_n] = is_dir;
        f_size[f_n] = 0;
        if (!is_dir) {
            u32 v = 0;
            while (*q && *q != '\n' && (*q < '0' || *q > '9')) q++;
            while (*q >= '0' && *q <= '9') v = v * 10 + (u32)(*q++ - '0');
            f_size[f_n] = v;
        }
        f_n++;
        while (*p && *p != '\n') p++;
        if (*p) p++;
    }
}

static void files_draw(void)
{
    panel(PX, PY, PW, PH, A_FRAME, " Files ");
    if (f_n == 0) {
        API->cputs(PX + 3, PY + 2, "(empty directory)", A_DIM);
        return;
    }
    int vis = PH - 5;                           /* interior minus hint row */
    if (f_sel < f_top) f_top = f_sel;
    if (f_sel >= f_top + vis) f_top = f_sel - vis + 1;

    for (int i = 0; i < vis && f_top + i < f_n; i++) {
        int idx = f_top + i;
        int sel = (idx == f_sel);
        u32 a = sel ? A_SEL : (f_dir[idx] ? A_LABEL : A_BODY);
        char row[44];
        int k = 0;
        row[k++] = sel ? NXP_CH_RARROW : ' ';
        row[k++] = '[';
        row[k++] = f_dir[idx] ? 'D' : 'F';
        row[k++] = ']';
        row[k++] = ' ';
        for (int j = 0; f_name[idx][j] && k < 24 && j < 19; j++) row[k++] = f_name[idx][j];
        if (!f_dir[idx]) {
            while (k < 30) row[k++] = ' ';
            row[k++] = '(';
            k = (int)(put_num(row + k, f_size[idx]) - row);
            row[k++] = ' '; row[k++] = 'B'; row[k++] = ')';
        }
        row[k] = 0;
        API->cellfill(PX + 1, PY + 2 + i, PW - 2, 1,
                      ((u32)' ' << 8) | (sel ? A_SEL : A_BODY));
        API->cputs(PX + 2, PY + 2 + i, row, a);
    }
    API->cputs(PX + 2, PY + PH - 2, "Enter: open   Backspace: parent dir", A_DIM);
}

/* viewer overlay; returns after any non-scroll key */
static void view_file(const char *name)
{
    char buf[VIEW_MAX + 1];                     /* stack: syscall buffer */
    char rowb[121];
    int sz = API->readfile(name, (unsigned char *)buf, VIEW_MAX);
    if (sz < 0) {
        int w = 40, h = 5;
        int vx = (COLS - w) / 2, vy = (ROWS - h) / 2;
        panel(vx, vy, w, h, A_ERR, " View ");
        API->cputs(vx + 2, vy + 2, "cannot read - press any key", A_ERR);
        int c;
        while ((c = API->getkey()) < 0) { }
        (void)c;
        return;
    }
    buf[sz] = 0;

    /* viewer owns the screen: clear to the desktop so nothing (e.g. the
     * selected row highlight) pokes out around the overlay */
    API->cellfill(0, 0, COLS, ROWS, ((u32)' ' << 8) | A_DESK);
    draw_bar();
    draw_status();

    int w = mini(COLS - 6, 100), h = mini(ROWS - 4, 30);
    int vx = (COLS - w) / 2, vy = (ROWS - h) / 2;
    int vis = h - 4;
    int top = 0;

    for (;;) {
        /* count lines and draw from 'top' */
        panel(vx, vy, w, h, A_FRAME, " View: ");
        API->cputs(vx + 9, vy, name, A_FRAME);
        int line = 0, off = 0, drawn = 0;
        while (off < sz && drawn < vis) {
            int lb2 = 0;
            while (off + lb2 < sz && buf[off + lb2] != '\n' && lb2 < 120)
                { rowb[lb2] = buf[off + lb2]; lb2++; }
            rowb[lb2] = 0;
            if (line >= top) {
                API->cellfill(vx + 1, vy + 2 + drawn, w - 2, 1,
                              ((u32)' ' << 8) | A_VIEW);
                API->cputs(vx + 2, vy + 2 + drawn, rowb, A_VIEW);
                drawn++;
            }
            off += lb2;
            if (off < sz && buf[off] == '\n') off++;
            line++;
        }
        for (int i = drawn; i < vis; i++)
            API->cellfill(vx + 1, vy + 2 + i, w - 2, 1, ((u32)' ' << 8) | A_VIEW);

        char pos[24];
        char *e = put_str(pos, "line ");
        e = put_num(e, mini(top + 1, line));    /* line = total seen */
        e = put_str(e, "/");
        e = put_num(e, line);
        *e = 0;
        API->cputs(vx + 2, vy + h - 2, pos, A_DIM);
        API->cputs(vx + w - 30, vy + h - 2, "Up/Down scroll   any other key: back", A_DIM);

        int c;
        while ((c = API->getkey()) < 0) { }
        if (c == NXP_KEY_UP && top > 0) top--;
        else if (c == NXP_KEY_DOWN && top + vis < line) top++;
        else return;
    }
}

static void files_key(int c)
{
    if (c == NXP_KEY_UP && f_sel > 0) { f_sel--; files_draw(); }
    else if (c == NXP_KEY_DOWN && f_sel < f_n - 1) { f_sel++; files_draw(); }
    else if (c == '\n' && f_n) {
        if (f_dir[f_sel]) {
            if (API->fsop(5, f_name[f_sel]) == 0) { files_load(); files_draw(); }
        } else {
            view_file(f_name[f_sel]);
            files_draw();                       /* wipe the viewer overlay */
        }
    }
    else if (c == '\b') {
        if (API->fsop(5, "..") == 0) { files_load(); files_draw(); }
    }
}

/* =============================== Tasks =============================== */
static char t_line[6][16];                      /* "1 r nsh.nxp" */
static int  t_n, t_sel;

static void tasks_load(void)
{
    char pb[512];                               /* stack: syscall buffer */
    t_n = 0;
    API->procs(pb, (u32)sizeof pb);
    const char *p = pb;
    while (*p && t_n < 6) {
        int k = 0;
        while (*p && *p != '\n' && k < 15) t_line[t_n][k++] = *p++;
        t_line[t_n][k] = 0;
        if (k) t_n++;
        if (*p) p++;
    }
    if (t_sel >= t_n) t_sel = t_n ? t_n - 1 : 0;
}

static void tasks_draw(void)
{
    panel(PX, PY, PW, PH, A_FRAME, " Tasks ");
    if (!t_n)
        API->cputs(PX + 3, PY + 2, "no processes (just this TUI)", A_DIM);
    int vis = PH - 5;
    for (int i = 0; i < vis; i++) {
        API->cellfill(PX + 1, PY + 2 + i, PW - 2, 1, ((u32)' ' << 8) | A_BODY);
        if (i >= t_n) continue;
        int sel = (i == t_sel);
        u32 a = sel ? A_SEL : A_BODY;
        char row[24];
        int k = 0;
        row[k++] = sel ? NXP_CH_RARROW : ' ';
        row[k++] = ' ';
        for (int j = 0; t_line[i][j] && k < 22; j++) row[k++] = t_line[i][j];
        row[k] = 0;
        API->cputs(PX + 2, PY + 2 + i, row, a);
    }
    API->cputs(PX + 2, PY + PH - 2, "k: kill selected (not the TUI itself)", A_DIM);
}

static void tasks_key(int c)
{
    if (c == NXP_KEY_UP && t_sel > 0) { t_sel--; tasks_draw(); }
    else if (c == NXP_KEY_DOWN && t_sel < t_n - 1) { t_sel++; tasks_draw(); }
    else if (c == 'k' && t_n) {
        /* refuse to kill the TUI's own entry (name starts with "tui") */
        int i = 0;
        while (t_line[t_sel][i] == ' ') i++;
        while (t_line[t_sel][i] && t_line[t_sel][i] != ' ') i++;
        while (t_line[t_sel][i] == ' ') i++;
        if (t_line[t_sel][i] == 't' && t_line[t_sel][i+1] == 'u' &&
            t_line[t_sel][i+2] == 'i') return;
        char pid[2];
        pid[0] = t_line[t_sel][0];
        pid[1] = 0;
        if (pid[0] >= '1' && pid[0] <= '4') {
            API->sysop(11, pid);
            tasks_load();
            tasks_draw();
        }
    }
}

/* ============================== System =============================== */
static void sys_draw(void)
{
    char b[80];                                 /* stack: syscall buffer */
    int row = PY + 2;
    panel(PX, PY, PW, PH, A_FRAME, " System ");
    u32 up = API->ticks() / 100;                /* seconds since boot */
    u32 hh = up / 3600, mm = (up / 60) % 60, ss = up % 60;

    API->cputs(PX + 4, row, "User    : ", A_LABEL);
    if (API->getuser(b, (u32)sizeof b) > 0) API->cputs(PX + 16, row, b, A_BODY);
    row += 2;

    API->cputs(PX + 4, row, "Date    : ", A_LABEL);
    if (API->getdate(b, (u32)sizeof b) > 0) API->cputs(PX + 16, row, b, A_BODY);
    row += 2;

    API->cputs(PX + 4, row, "Uptime  : ", A_LABEL);
    {
        char *e = put_num(b, hh);
        *e++ = ':';
        if (mm < 10) *e++ = '0';
        e = put_num(e, mm);
        *e++ = ':';
        if (ss < 10) *e++ = '0';
        e = put_num(e, ss);
        *e = 0;
    }
    API->cputs(PX + 16, row, b, A_BODY);
    row += 2;

    API->cputs(PX + 4, row, "Screen  : ", A_LABEL);
    {
        char *e = put_num(b, (u32)COLS);
        *e++ = 'x';
        e = put_num(e, (u32)ROWS);
        e = put_str(e, API->scr_w ? " cells, LFB" : " cells, VGA");
        *e = 0;
    }
    API->cputs(PX + 16, row, b, A_BODY);
    row += 2;

    API->cputs(PX + 4, row, "Tasks   : ", A_LABEL);
    {
        char *e = put_num(b, (u32)t_n);
        *e = 0;
    }
    API->cputs(PX + 16, row, b, A_BODY);
}

/* ============================== main ================================= */
static void draw_screen(void)
{
    API->cellfill(0, 0, COLS, ROWS, ((u32)' ' << 8) | A_DESK);
    draw_bar();
    if (g_tab == 0) { files_load(); files_draw(); }
    else if (g_tab == 1) { tasks_load(); tasks_draw(); }
    else sys_draw();
    draw_status();
}

void nxp_main(void)
{
    COLS = API->scr_w ? (int)(API->scr_w / 8) : 80;
    ROWS = API->scr_h ? (int)(API->scr_h / 16) : 25;
    PX = 1; PY = 1;
    PW = COLS - 2; PH = ROWS - 2;
    g_tab = 0;
    f_sel = f_top = f_n = 0;
    t_n = t_sel = 0;

    API->cursor(0);
    draw_screen();

    u32 last_sec = API->ticks() / 100;
    for (;;) {
        int c = API->getkey();
        if (c < 0) {
            u32 sec = API->ticks() / 100;
            if (sec != last_sec) {              /* 1 Hz: clock + tasks refresh */
                last_sec = sec;
                draw_bar();
                if (g_tab == 1) { tasks_load(); tasks_draw(); }
                draw_status();
            }
            continue;
        }
        if (c == 'q' || c == 'Q' || c == 27) break;
        if (c == '1' || c == NXP_KEY_LEFT)       { g_tab = 0; draw_screen(); }
        else if (c == '2')                       { g_tab = 1; draw_screen(); }
        else if (c == '3' || c == NXP_KEY_RIGHT) { g_tab = 2; draw_screen(); }
        else if (g_tab == 0) files_key(c);
        else if (g_tab == 1) tasks_key(c);
    }
    API->cursor(1);
    API->exit();
}
