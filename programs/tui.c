/* ============================================================
 * tui.nxp - NovaOS fullscreen TUI (Ring 3), modern dark theme
 *
 * Three panels on top of the cell-addressed console syscalls
 * (putcell / cellfill / cputs - see nxp.h):
 *
 *   [1] Files  - NovaFS browser with a live PREVIEW pane: arrows
 *                move, Enter opens a directory or views a file
 *                (fullscreen viewer), Backspace goes up
 *   [2] Tasks  - live process list, k kills the selected pid
 *   [3] System - user / date / uptime / screen info
 *
 * Keys: 1/2/3 or Left/Right switch panels, q or Esc quits.
 * Theme: near-black background, dim-gray chrome, cyan accents;
 * thin single-line frames, pill tabs, list glyphs, scrollbars.
 * The preview pane appears only when COLS >= 100 (VBE console);
 * the 80x25 VGA fallback runs a single pane.
 *
 * Buffer rule: every buffer handed to a syscall (listdir/procs/
 * readfile/getdate/getuser) lives on the STACK - the kernel only
 * accepts user pointers from the 32KB slot-image window or the
 * 32KB stack window; keeping the big buffers on the stack also
 * frames them under 4KB so MinGW never emits __chkstk_ms.
 * ============================================================ */
#include "nxp.h"

/* ---- theme (VGA attribute bytes: fg | bg<<4) ----
 * The accent is user-configurable (Settings panel), so accent-colored
 * attrs come from acc()/acc_sel() instead of constants. */
#define A_APP    NXP_ATTR(NXP_COLOR_WHITE,   NXP_COLOR_BLACK)   /* header brand */
#define A_HDR    NXP_ATTR(NXP_COLOR_LGRAY,   NXP_COLOR_BLACK)   /* clock / user */
#define A_RULE   NXP_ATTR(NXP_COLOR_DGRAY,   NXP_COLOR_BLACK)   /* chrome/frame */
#define A_TABON  NXP_ATTR(NXP_COLOR_WHITE,   NXP_COLOR_DGRAY)   /* active pill  */
#define A_TABOFF NXP_ATTR(NXP_COLOR_DGRAY,   NXP_COLOR_BLACK)
#define A_TXT    NXP_ATTR(NXP_COLOR_LGRAY,   NXP_COLOR_BLACK)
#define A_DIM    NXP_ATTR(NXP_COLOR_DGRAY,   NXP_COLOR_BLACK)
#define A_SEL    NXP_ATTR(NXP_COLOR_WHITE,   NXP_COLOR_DGRAY)   /* selected row */
#define A_OK     NXP_ATTR(NXP_COLOR_LGREEN,  NXP_COLOR_BLACK)
#define A_WARN   NXP_ATTR(NXP_COLOR_YELLOW,  NXP_COLOR_BLACK)
#define A_ERR    NXP_ATTR(NXP_COLOR_LRED,    NXP_COLOR_BLACK)
#define A_VIEW   NXP_ATTR(NXP_COLOR_LGRAY,   NXP_COLOR_BLACK)

/* ---- settings (persisted to nova.cfg - the GLOBAL config the kernel
 * also reads at boot: accent recolors the shell, quiet hides the
 * banner/self-test; the rest are TUI-only) ---- */
static int s_accent;    /* 0 cyan 1 green 2 yellow 3 magenta 4 white  */
static int s_quiet;    /* 0 off 1 on (global: no banner/self-test)    */
static int s_preview;   /* 0 auto 1 on 2 off                          */
static int s_clock;     /* 0 show 1 hide                              */
static int s_human;     /* 0 bytes 1 human (7.6K)                     */
static int s_selbar;    /* 0 row highlight 1 accent bar               */
static int s_sel;       /* selected row in the Settings panel         */

static const char *acc_names[5] = { "cyan", "green", "yellow", "magenta", "white" };
static const u8    acc_fg[5]    = { NXP_COLOR_LCYAN, NXP_COLOR_LGREEN, NXP_COLOR_YELLOW,
                                     NXP_COLOR_LMAGENTA, NXP_COLOR_WHITE };
static const char *quiet_names[2] = { "off", "on" };
static u32 acc(void)     { return NXP_ATTR(acc_fg[s_accent], NXP_COLOR_BLACK); }
static u32 acc_sel(void) { return NXP_ATTR(acc_fg[s_accent], NXP_COLOR_DGRAY); }

static int WIDE;        /* COLS >= 100: preview pane possible at all  */
static int prev_eff(void)
{
    if (s_preview == 1) return 1;
    if (s_preview == 2 || !WIDE) return 0;
    return 1;                                   /* auto */
}

/* ---- layout (computed at start) ---- */
#define VIEW_MAX 3072                    /* viewer cap; keeps frames < 4KB */
static int COLS, ROWS;
static int LX, LY, LW, LH;               /* left/main panel                */
static int RX, RY, RW, RH;               /* preview panel (if any)         */
static int PREVIEW_ON;
static int TABX0;                        /* first tab pill cell            */
static int g_tab;
static int CW;                           /* current panel width (Tasks and
                                          * System span the full width)   */

/* ---- files state (never passed to syscalls: may live in BSS) ---- */
#define MAXENT 64
static char f_name[MAXENT][24];
static int  f_dir[MAXENT];
static u32  f_size[MAXENT];
static int  f_n, f_sel, f_top;

/* ---- tasks state ---- */
static char t_name[6][14];
static char t_pid[6];
static char t_st[6];
static int  t_n, t_sel;

/* ---- forward decls ---- */
static void preview_draw(void);
static void view_file(const char *name);
static const char *keys_hint(int tab);
static void layout(void);
static void draw_screen(void);
static void settings_key(int c);

/* ---- generic helpers ---- */
static int mini(int a, int b) { return a < b ? a : b; }

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
static int str_len(const char *s) { int n = 0; while (s[n]) n++; return n; }

/* text right-aligned ending at (xend) */
static void puts_right(int xend, int y, const char *s, u32 attr)
{
    API->cputs(xend - str_len(s), y, s, attr);
}

static void draw_hline(int x, int y, int w, u32 attr, char ch)
{
    API->cellfill(x, y, w, 1, ((u32)(ch & 0xFF) << 8) | attr);
}

/* thin single-line frame with an embedded title */
static void panel(int x, int y, int w, int h, const char *title)
{
    API->cellfill(x, y, w, h, ((u32)' ' << 8) | A_TXT);
    draw_hline(x + 1, y, w - 2, A_RULE, NXP_CH_H_S);
    draw_hline(x + 1, y + h - 1, w - 2, A_RULE, NXP_CH_H_S);
    API->cellfill(x, y + 1, 1, h - 2, ((u32)NXP_CH_V_S << 8) | A_RULE);
    API->cellfill(x + w - 1, y + 1, 1, h - 2, ((u32)NXP_CH_V_S << 8) | A_RULE);
    API->putcell(x, y, NXP_CH_TL_S, A_RULE);
    API->putcell(x + w - 1, y, NXP_CH_TR_S, A_RULE);
    API->putcell(x, y + h - 1, NXP_CH_BL_S, A_RULE);
    API->putcell(x + w - 1, y + h - 1, NXP_CH_BR_S, A_RULE);
    if (title && title[0]) API->cputs(x + 2, y, title, acc());
}

/* vertical scrollbar: dim ░ track, cyan █ thumb */
static void scrollbar(int x, int y, int h, int total, int top, int vis)
{
    if (total <= vis || total <= 0) return;
    int thumb = vis * vis / total;
    if (thumb < 1) thumb = 1;
    int maxpos = total - vis;
    int tpos = (maxpos == 0) ? 0 : (top * (h - thumb) + maxpos / 2) / maxpos;
    for (int r = 0; r < h; r++) {
        int inthumb = (r >= tpos && r < tpos + thumb);
        API->putcell(x, y + r, inthumb ? NXP_CH_BLOCK : NXP_CH_SHADE,
                     inthumb ? acc() : A_RULE);
    }
}

/* ============================ header/status ============================ */
static const char *tab_name[4] = { " 1 Files ", " 2 Tasks ", " 3 System ", " 4 Settings " };

static const char *keys_hint(int tab)
{
    if (tab == 0) return "enter open   bksp up   1-4 panels   q quit";
    if (tab == 1) return "up/down pick   k kill   1-4 panels   q quit";
    if (tab == 2) return "1-4 panels   q quit";
    return "up/down select   left/right change   q quit";
}

static void draw_bar(void)
{
    char db[64];                                /* stack: syscall buffer */
    API->cellfill(0, 0, COLS, 1, ((u32)' ' << 8) | A_TXT);
    API->cputs(1, 0, "NovaOS", A_APP);

    TABX0 = COLS / 2 - 22;
    if (TABX0 < 12) TABX0 = 12;
    for (int t = 0; t < 4; t++)
        API->cputs(TABX0 + t * 11, 0, tab_name[t],
                   (t == g_tab) ? A_TABON : A_TABOFF);

    if (s_clock == 0 && API->getdate(db, (u32)sizeof db) > 0) {
        for (int i = 0; db[i]; i++) {
            if (db[i] == 'T' && db[i+1] == 'i' && db[i+2] == 'm' && db[i+3] == 'e') {
                const char *tm = db + i + 6;
                int len = 0;
                while (tm[len] >= ' ' && len < 9) len++;
                while (len && tm[len-1] == ' ') len--;
                if (len) puts_right(COLS - 1, 0, tm, A_HDR);
                break;
            }
        }
    }
    draw_hline(0, 1, COLS, A_RULE, NXP_CH_H_S);
}

static void draw_status(void)
{
    char ub[24];                                /* stack: syscall buffer */
    API->cellfill(0, ROWS - 1, COLS, 1, ((u32)' ' << 8) | A_TXT);
    API->cputs(1, ROWS - 1, keys_hint(g_tab), A_DIM);
    if (API->getuser(ub, (u32)sizeof ub) > 0) {
        int len = 0;
        while (ub[len] && len < 14) len++;
        puts_right(COLS - 2, ROWS - 1, ub, A_HDR);
    }
}

/* ============================== Settings ============================= */
static const char *prev_names[3]  = { "auto", "on", "off" };
static const char *clock_names[2] = { "show", "hide" };
static const char *size_names[2]  = { "bytes", "human" };
static const char *sel_names[2]   = { "row", "bar" };

static int str_eq(const char *a, const char *b)
{
    while (*a && *a == *b) { a++; b++; }
    return (*a == 0 && *b == 0);
}

static void cfg_set(const char *k, const char *v)
{
    if (str_eq(k, "accent")) {
        for (int i = 0; i < 5; i++) if (str_eq(v, acc_names[i])) s_accent = i;
    } else if (str_eq(k, "quiet")) {
        for (int i = 0; i < 2; i++) if (str_eq(v, quiet_names[i])) s_quiet = i;
    } else if (str_eq(k, "preview")) {
        for (int i = 0; i < 3; i++) if (str_eq(v, prev_names[i])) s_preview = i;
    } else if (str_eq(k, "clock")) {
        for (int i = 0; i < 2; i++) if (str_eq(v, clock_names[i])) s_clock = i;
    } else if (str_eq(k, "sizes")) {
        for (int i = 0; i < 2; i++) if (str_eq(v, size_names[i])) s_human = i;
    } else if (str_eq(k, "selection")) {
        for (int i = 0; i < 2; i++) if (str_eq(v, sel_names[i])) s_selbar = i;
    }
}

static void load_settings(void)
{
    char buf[256];                              /* stack: syscall buffer */
    s_accent = 0; s_quiet = 0; s_preview = 0; s_clock = 0; s_human = 0; s_selbar = 0;
    int sz = API->readfile("nova.cfg", (unsigned char *)buf, (u32)sizeof buf - 1);
    if (sz <= 0) return;
    buf[sz] = 0;
    char *p = buf;
    while (*p) {
        char *line = p;
        while (*p && *p != '\n') p++;
        if (*p) *p++ = 0;
        char *eq = line;
        while (*eq && *eq != '=') eq++;
        if (!*eq) continue;
        *eq = 0;
        cfg_set(line, eq + 1);
    }
}

static int save_settings(void)
{
    char buf[192];                              /* stack: syscall buffer */
    char *e = buf;
    e = put_str(e, "accent=");   e = put_str(e, acc_names[s_accent]);   e = put_str(e, "\n");
    e = put_str(e, "quiet=");    e = put_str(e, quiet_names[s_quiet]);  e = put_str(e, "\n");
    e = put_str(e, "preview=");  e = put_str(e, prev_names[s_preview]); e = put_str(e, "\n");
    e = put_str(e, "clock=");    e = put_str(e, clock_names[s_clock]);  e = put_str(e, "\n");
    e = put_str(e, "sizes=");    e = put_str(e, size_names[s_human]);   e = put_str(e, "\n");
    e = put_str(e, "selection=");e = put_str(e, sel_names[s_selbar]);   e = put_str(e, "\n");
    *e = 0;
    return API->writefile("nova.cfg", (unsigned char *)buf, (u32)(e - buf));
}

/* "464 B" / "7.6 K" */
static void fmt_size(char *out, u32 v)
{
    if (!s_human || v < 1024) {
        char *e = put_num(out, v);
        e = put_str(e, " B"); *e = 0;
        return;
    }
    char *e = put_num(out, v / 1024);
    *e++ = '.';
    *e++ = (char)('0' + (v % 1024) * 10 / 1024);
    e = put_str(e, " K"); *e = 0;
}

static int s_save_ok;                   /* last settings save succeeded? */

static void settings_apply(int delta)
{
    switch (s_sel) {
    case 0:
        s_accent = (s_accent + delta + 5) % 5;
        break;
    case 1:
        s_quiet = (s_quiet + 1) % 2;            /* takes effect next boot */
        break;
    case 2:
        s_preview = (s_preview + delta + 3) % 3;
        layout();
        break;
    case 3:
        s_clock = (s_clock + 1) % 2;
        break;
    case 4:
        s_human = (s_human + 1) % 2;
        break;
    default:
        s_selbar = (s_selbar + 1) % 2;
        break;
    }
    s_save_ok = (save_settings() >= 0);
    API->sysop(18, "");                         /* kernel re-reads nova.cfg */
    draw_screen();
}

static void settings_key(int c)
{
    if (c == NXP_KEY_UP && s_sel > 0) { s_sel--; draw_screen(); }
    else if (c == NXP_KEY_DOWN && s_sel < 5) { s_sel++; draw_screen(); }
    else if (c == NXP_KEY_LEFT || c == '-')  settings_apply(-1);
    else if (c == NXP_KEY_RIGHT || c == '+') settings_apply(1);
}

static void settings_draw(void)
{
    panel(LX, LY, CW, LH, " Settings ");
    static const char *labels[6] = { "Accent", "Quiet boot", "Preview pane",
                                     "Header clock", "Size format", "Selection" };
    int rowsy = LY + 2;
    for (int i = 0; i < 6; i++) {
        int rowy = rowsy + i * 2;
        int sel = (i == s_sel);
        API->cellfill(LX + 1, rowy, CW - 2, 1, ((u32)' ' << 8) | A_TXT);
        API->cputs(LX + 4, rowy, labels[i], sel ? A_SEL : A_TXT);

        const char *val = (i == 0) ? acc_names[s_accent]
                        : (i == 1) ? quiet_names[s_quiet]
                        : (i == 2) ? prev_names[s_preview]
                        : (i == 3) ? clock_names[s_clock]
                        : (i == 4) ? size_names[s_human]
                                   : sel_names[s_selbar];
        char pill[28];
        char *e = put_str(pill, "[ ");
        e = put_str(e, val);
        if (i == 1 && s_quiet) e = put_str(e, " (next boot)");
        if (i == 2 && !WIDE) e = put_str(e, " (n/a)");
        e = put_str(e, " ]"); *e = 0;
        /* the accent row doubles as a color swatch */
        API->cputs(LX + 22, rowy, pill, (i == 0) ? acc() : (sel ? A_SEL : A_TXT));
        if (sel) {
            API->cputs(LX + 20, rowy, "<", A_DIM);
            API->cputs(LX + 23 + str_len(pill), rowy, ">", A_DIM);
        }
    }
    if (s_save_ok)
        API->cputs(LX + 3, LY + LH - 2,
                   "accent + quiet are global (kernel reads nova.cfg)", A_DIM);
    else
        API->cputs(LX + 3, LY + LH - 2,
                   "write DENIED - nova.cfg owned by another user", A_ERR);
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
        else {
            while (*p && *p != '\n') p++;
            if (*p) p++;
            continue;
        }

        /* tag(9) + mode "rwxrwx"(6) + space(1) -> name starts at +16 */
        const char *s = p + 16;
        const char *q = s;
        while (*q && *q != '\n' && *q != '(') q++;
        while (q > s && q[-1] == ' ') q--;

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
    panel(LX, LY, LW, LH, " Files ");

    int rowsy = LY + 2;
    int vis = LH - 4;

    /* dim table header */
    API->cputs(LX + 4, LY + 1, "NAME", A_DIM);
    puts_right(LX + LW - 4, LY + 1, "SIZE", A_DIM);

    if (f_n == 0) {
        API->cputs(LX + 4, rowsy, "(empty directory)", A_DIM);
        return;
    }
    if (f_sel < f_top) f_top = f_sel;
    if (f_sel >= f_top + vis) f_top = f_sel - vis + 1;

    for (int i = 0; i < vis && f_top + i < f_n; i++) {
        int idx = f_top + i;
        int sel = (idx == f_sel);
        int rowy = rowsy + i;
        int bar = (sel && s_selbar);            /* accent-bar selection */
        API->cellfill(LX + 1, rowy, LW - 2, 1,
                      ((u32)' ' << 8) | (sel ? (bar ? A_TXT : A_SEL) : A_TXT));
        if (sel) API->putcell(LX + 2, rowy, NXP_CH_BLOCK,
                              bar ? acc() : acc_sel());   /* block = selected row */
        else API->putcell(LX + 2, rowy,
                          f_dir[idx] ? NXP_CH_RARROW : NXP_CH_DOT,
                          f_dir[idx] ? acc() : A_DIM);
        API->cputs(LX + 4, rowy, f_name[idx],
                   sel ? A_SEL : (f_dir[idx] ? acc() : A_TXT));
        if (!f_dir[idx]) {
            char num[12];
            fmt_size(num, f_size[idx]);
            puts_right(LX + LW - 4, rowy, num, sel ? A_SEL : A_DIM);
        }
    }
    scrollbar(LX + LW - 2, rowsy, vis, f_n, f_top, vis);
    preview_draw();
}

/* preview pane for the selected entry */
static void preview_draw(void)
{
    if (!prev_eff()) return;
    panel(RX, RY, RW, RH, " Preview ");

    if (!f_n) { API->cputs(RX + 3, RY + 2, "nothing selected", A_DIM); return; }

    if (f_dir[f_sel]) {
        char b[48];
        char *e = put_str(b, f_name[f_sel]);
        e = put_str(e, " - directory"); *e = 0;
        API->cputs(RX + 3, RY + 2, b, acc());
        char cnt[16];
        char *ce = put_num(cnt, (u32)f_n);
        *ce = 0;
        e = put_str(b, cnt); e = put_str(e, " item(s) listed"); *e = 0;
        API->cputs(RX + 3, RY + 3, b, A_DIM);
        API->cputs(RX + 3, RY + 5, "enter opens it", A_DIM);
        return;
    }

    char buf[VIEW_MAX + 1];                     /* stack: syscall buffer */
    char rowb[100];
    int sz = API->readfile(f_name[f_sel], (unsigned char *)buf, VIEW_MAX);
    if (sz < 0) { API->cputs(RX + 3, RY + 2, "cannot read", A_ERR); return; }
    buf[sz] = 0;

    /* NXP binaries would render as garbage - show a summary instead */
    if (sz >= 4 && buf[0] == 'N' && buf[1] == 'X' && buf[2] == 'P') {
        API->cputs(RX + 3, RY + 2, "NXP program binary", A_OK);
        char b2[40]; char *e2 = put_str(b2, "size ");
        e2 = put_num(e2, f_size[f_sel]); *e2 = 0;
        API->cputs(RX + 3, RY + 3, b2, A_DIM);
        API->cputs(RX + 3, RY + 5, "run it from the shell:", A_DIM);
        char b3[48]; char *e3 = put_str(b3, "  run ");
        e3 = put_str(e3, f_name[f_sel]); *e3 = 0;
        API->cputs(RX + 3, RY + 6, b3, A_TXT);
        return;
    }

    int vis = RH - 4;
    int line = 0, off = 0, drawn = 0;
    while (off < sz && drawn < vis) {
        int lb2 = 0;
        while (off + lb2 < sz && buf[off + lb2] != '\n' && lb2 < 90)
            { rowb[lb2] = buf[off + lb2]; lb2++; }
        rowb[lb2] = 0;
        API->cellfill(RX + 1, RY + 2 + drawn, RW - 2, 1, ((u32)' ' << 8) | A_TXT);
        char num[8];
        char *ne = put_num(num, (u32)(line + 1));
        *ne = 0;
        API->cputs(RX + 3, RY + 2 + drawn, num, A_DIM);
        API->cputs(RX + 8, RY + 2 + drawn, rowb, A_TXT);
        drawn++;
        off += lb2;
        if (off < sz && buf[off] == '\n') off++;
        line++;
    }
    scrollbar(RX + RW - 2, RY + 2, vis, line, 0, vis);
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
            files_draw();
        }
    }
    else if (c == '\b') {
        if (API->fsop(5, "..") == 0) { files_load(); files_draw(); }
    }
}

/* =============================== Tasks =============================== */
static void tasks_load(void)
{
    char pb[512];                               /* stack: syscall buffer */
    t_n = 0;
    API->procs(pb, (u32)sizeof pb);
    const char *p = pb;
    while (*p && t_n < 6) {
        t_pid[t_n] = ' '; t_st[t_n] = ' ';
        t_name[t_n][0] = 0;
        int k = 0;
        while (*p && *p != '\n') {
            char ch = *p++;
            if (k == 0) t_pid[t_n] = ch;
            else if (k == 2) t_st[t_n] = ch;
            else if (k >= 4 && k - 4 < 13) t_name[t_n][k - 4] = ch;
            k++;
        }
        t_name[t_n][(k > 4) ? ((k - 4 < 13) ? k - 4 : 13) : 0] = 0;
        if (k) t_n++;
        if (*p) p++;
    }
    if (t_sel >= t_n) t_sel = t_n ? t_n - 1 : 0;
}

static void tasks_draw(void)
{
    panel(LX, LY, CW, LH, " Tasks ");
    int rowsy = LY + 2;
    int vis = LH - 4;
    if (!t_n)
        API->cputs(LX + 4, rowsy, "no processes (just this TUI)", A_DIM);
    for (int i = 0; i < vis; i++) {
        API->cellfill(LX + 1, rowsy + i, CW - 2, 1, ((u32)' ' << 8) | A_TXT);
        if (i >= t_n) continue;
        int sel = (i == t_sel);
        char row[40];
        char *e = row;
        *e++ = sel ? NXP_CH_RARROW : ' ';
        *e++ = ' ';
        *e++ = t_pid[i];
        *e++ = ' ';
        *e++ = NXP_CH_BULLET;
        *e++ = ' ';
        *e++ = t_st[i];
        *e++ = ' '; *e++ = ' ';
        for (int j = 0; t_name[i][j] && e - row < 38; j++) *e++ = t_name[i][j];
        *e = 0;
        API->cputs(LX + 2, rowsy + i, row, sel ? A_SEL : A_TXT);
        /* colored state dot: green = ready, yellow = suspended */
        API->putcell(LX + 6, rowsy + i, NXP_CH_BULLET,
                     (t_st[i] == 'r') ? A_OK : A_WARN);
    }
    scrollbar(LX + CW - 2, rowsy, vis, t_n, 0, vis);
    API->cputs(LX + 3, LY + LH - 2,
               "k kills the selected process (never the TUI itself)", A_DIM);
}

static void tasks_key(int c)
{
    if (c == NXP_KEY_UP && t_sel > 0) { t_sel--; tasks_draw(); }
    else if (c == NXP_KEY_DOWN && t_sel < t_n - 1) { t_sel++; tasks_draw(); }
    else if (c == 'k' && t_n) {
        /* refuse to kill the TUI's own entry (name starts with "tui") */
        if (t_name[t_sel][0] == 't' && t_name[t_sel][1] == 'u' &&
            t_name[t_sel][2] == 'i') return;
        char pid[2];
        pid[0] = t_pid[t_sel];
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
    panel(LX, LY, CW, LH, " System ");
    int row = LY + 2;
    u32 up = API->ticks() / 100;                /* seconds since boot */
    u32 hh = up / 3600, mm = (up / 60) % 60, ss = up % 60;

    API->cputs(LX + 4, row, "User", A_DIM);
    if (API->getuser(b, (u32)sizeof b) > 0) API->cputs(LX + 18, row, b, A_TXT);
    row += 2;

    API->cputs(LX + 4, row, "Date", A_DIM);
    if (API->getdate(b, (u32)sizeof b) > 0) API->cputs(LX + 18, row, b, A_TXT);
    row += 2;

    API->cputs(LX + 4, row, "Uptime", A_DIM);
    {
        char *e = b;
        e = put_num(e, hh); *e++ = ':';
        if (mm < 10) *e++ = '0';
        e = put_num(e, mm); *e++ = ':';
        if (ss < 10) *e++ = '0';
        e = put_num(e, ss); *e = 0;
    }
    API->cputs(LX + 18, row, b, A_TXT);
    row += 2;

    API->cputs(LX + 4, row, "Screen", A_DIM);
    {
        char *e = b;
        e = put_num(e, (u32)COLS); *e++ = 'x';
        e = put_num(e, (u32)ROWS);
        e = put_str(e, API->scr_w ? "  LFB" : "  VGA text");
        *e = 0;
    }
    API->cputs(LX + 18, row, b, A_TXT);
    row += 2;

    API->cputs(LX + 4, row, "Tasks", A_DIM);
    {
        char *e = put_num(b, (u32)t_n); *e = 0;
    }
    API->cputs(LX + 18, row, b, A_TXT);
    row += 2;

    API->cputs(LX + 4, row, "Files here", A_DIM);
    {
        char *e = put_num(b, (u32)f_n); *e = 0;
    }
    API->cputs(LX + 18, row, b, A_TXT);
}

/* ============================ file viewer ============================ */
static void view_file(const char *name)
{
    char buf[VIEW_MAX + 1];                     /* stack: syscall buffer */
    char rowb[100];
    int sz = API->readfile(name, (unsigned char *)buf, VIEW_MAX);
    if (sz < 0) {
        int w = 44, h = 5;
        int vx = (COLS - w) / 2, vy = (ROWS - h) / 2;
        panel(vx, vy, w, h, " Error ");
        API->cputs(vx + 3, vy + 2, "cannot read - press any key", A_ERR);
        int c;
        while ((c = API->getkey()) < 0) { }
        (void)c;
        return;
    }
    buf[sz] = 0;

    /* the viewer owns the screen */
    API->cellfill(0, 0, COLS, ROWS, ((u32)' ' << 8) | A_TXT);
    draw_bar();
    draw_status();

    int w = mini(COLS - 6, 110), h = mini(ROWS - 4, 34);
    int vx = (COLS - w) / 2, vy = (ROWS - h) / 2;
    int vis = h - 4;
    int top = 0;

    for (;;) {
        panel(vx, vy, w, h, " ");
        API->cputs(vx + 2, vy, name, acc());
        int line = 0, off = 0, drawn = 0;
        while (off < sz && drawn < vis) {
            int lb2 = 0;
            while (off + lb2 < sz && buf[off + lb2] != '\n' && lb2 < 90)
                { rowb[lb2] = buf[off + lb2]; lb2++; }
            rowb[lb2] = 0;
            API->cellfill(vx + 1, vy + 2 + drawn, w - 2, 1,
                          ((u32)' ' << 8) | A_TXT);
            char num[8];
            char *ne = put_num(num, (u32)(top + drawn + 1));
            *ne = 0;
            API->cputs(vx + 3, vy + 2 + drawn, num, A_DIM);
            API->cputs(vx + 8, vy + 2 + drawn, rowb, A_TXT);
            drawn++;
            off += lb2;
            if (off < sz && buf[off] == '\n') off++;
            line++;
        }
        for (int i = drawn; i < vis; i++)
            API->cellfill(vx + 1, vy + 2 + i, w - 2, 1, ((u32)' ' << 8) | A_TXT);
        scrollbar(vx + w - 2, vy + 2, vis, line, top, vis);

        char pos[24];
        char *e = put_str(pos, "line ");
        e = put_num(e, mini(top + 1, line));
        e = put_str(e, "/");
        e = put_num(e, line);
        *e = 0;
        API->cputs(vx + 3, vy + h - 2, pos, A_DIM);
        API->cputs(vx + w - 34, vy + h - 2,
                   "up/down scroll   any other key: back", A_DIM);

        int c;
        while ((c = API->getkey()) < 0) { }
        if (c == NXP_KEY_UP && top > 0) top--;
        else if (c == NXP_KEY_DOWN && top + vis < line) top++;
        else return;
    }
}

static void layout(void)
{
    LW = prev_eff() ? 58 : (COLS - 4);
    RX = LX + LW + 2;
    RW = COLS - RX - 2;
}

/* ============================== main ================================= */
static void draw_screen(void)
{
    API->cellfill(0, 0, COLS, ROWS, ((u32)' ' << 8) | A_TXT);
    draw_bar();
    CW = (g_tab == 0) ? LW : (COLS - 4);
    if (g_tab == 0) { files_load(); files_draw(); }
    else if (g_tab == 1) { tasks_load(); tasks_draw(); }
    else if (g_tab == 2) sys_draw();
    else settings_draw();
    draw_status();
}

void nxp_main(void)
{
    COLS = API->scr_w ? (int)(API->scr_w / 8) : 80;
    ROWS = API->scr_h ? (int)(API->scr_h / 16) : 25;
    WIDE = (COLS >= 100);

    API->fsop(5, "/");                          /* cfg lives in the root */
    LX = 2; LY = 3;
    LH = ROWS - 7;                              /* header 2 + status 2 + gap */
    RY = LY; RH = LH;

    load_settings();                            /* nova.cfg (defaults if new) */
    layout();

    g_tab = 0;
    f_sel = f_top = f_n = 0;
    t_n = t_sel = 0;

    API->cursor(0);
    draw_screen();

    u32 last_sec = API->ticks() / 100;
    for (;;) {
        int c = API->getkey();
        if (c < 0) {
            /* wheel scrolls the file list (and the Tasks selection) */
            int mdx, mdy, mb, mw;
            if (API->mouse(&mdx, &mdy, &mb, &mw) && mw) {
                if (g_tab == 0 && f_n) {
                    f_sel += mw;                       /* +up / -down */
                    if (f_sel < 0) f_sel = 0;
                    if (f_sel > f_n - 1) f_sel = f_n - 1;
                    files_draw();
                } else if (g_tab == 1 && t_n) {
                    t_sel += mw;
                    if (t_sel < 0) t_sel = 0;
                    if (t_sel > t_n - 1) t_sel = t_n - 1;
                    tasks_draw();
                }
            }
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
        if (c >= '1' && c <= '4') { g_tab = c - '1'; layout(); draw_screen(); }
        else if (g_tab == 3) settings_key(c);
        else if (c == NXP_KEY_LEFT)  { g_tab = (g_tab + 3) % 4; layout(); draw_screen(); }
        else if (c == NXP_KEY_RIGHT) { g_tab = (g_tab + 1) % 4; layout(); draw_screen(); }
        else if (g_tab == 0) files_key(c);
        else if (g_tab == 1) tasks_key(c);
    }
    API->cursor(1);
    API->exit();
}
