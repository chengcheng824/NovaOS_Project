/* ============================================================
 * NovaOS - feature subsystem (see feature.h)
 *
 * Config file: /etc/features.conf, one "name=enable|disable" per
 * line. NovaFS has no path syntax, so every file access here saves
 * the caller's cwd, cd's into the /etc directory inode (created on
 * demand), does the I/O, and restores the cwd - the rest of the
 * kernel never notices.
 * ============================================================ */
#include "feature.h"
#include "novafs.h"
#include "stdint.h"

/* ---- console output (kernel.c) ---- */
extern void kput(char c);
extern void kputs(const char *s);

static int nx_str_eq(const char *a, const char *b)
{
    while (*a && *b) { if (*a != *b) return 0; a++; b++; }
    return *a == *b;
}
static int nx_str_len(const char *s) { int n = 0; while (s[n]) n++; return n; }
static void nx_cpy(char *d, const char *s, int max)
{
    int i = 0;
    while (s[i] && i < max - 1) { d[i] = s[i]; i++; }
    d[i] = 0;
}

/* ---- feature table ---- */
typedef struct {
    char name[FEAT_NAME_MAX];
    uint8_t enabled;
} feat_t;

static feat_t feats[FEAT_MAX];
static int nfeats;
static int etc_dir = -1;        /* inode of /etc, -1 = not available */

static const char *builtin_names[2] = { "holiday_module", "cn_lang_support" };

static feat_t *feat_find(const char *name)
{
    for (int i = 0; i < nfeats; i++)
        if (nx_str_eq(feats[i].name, name)) return &feats[i];
    return 0;
}

static feat_t *feat_add(const char *name, int enabled)
{
    if (nfeats >= FEAT_MAX) return 0;
    if (nx_str_len(name) >= FEAT_NAME_MAX) return 0;
    feat_t *f = &feats[nfeats++];
    nx_cpy(f->name, name, FEAT_NAME_MAX);
    f->enabled = (uint8_t)(enabled ? 1 : 0);
    return f;
}

/* ---------------- /etc access ----------------
 * NovaFS is cwd-relative, so temporarily switch into /etc. */

static int etc_enter(int saved[1])
{
    if (!fs_is_ready()) return 0;
    saved[0] = fs_cwd();
    if (etc_dir >= 0 && fs_type_of(etc_dir) == T_DIR) {
        fs_setcwd(etc_dir);
        return 1;
    }
    /* find or create /etc (root dir) */
    fs_setcwd(0);
    int idx = fs_find("etc");
    if (idx < 0) {
        if (fs_mkdir("etc") != 0) { fs_setcwd(saved[0]); return 0; }
        idx = fs_find("etc");
    }
    if (idx < 0) { fs_setcwd(saved[0]); return 0; }
    etc_dir = idx;
    fs_setcwd(etc_dir);
    return 1;
}

static void etc_leave(const int saved[1])
{
    fs_setcwd(saved[0]);
}

/* ---------------- config I/O ---------------- */

static void cfg_parse(char *buf)
{
    char *p = buf;
    while (*p) {
        char *line = p;
        while (*p && *p != '\n') p++;
        if (*p) *p++ = 0;
        char *eq = line;
        while (*eq && *eq != '=') eq++;
        if (!*eq) continue;
        *eq = 0;
        char *val = eq + 1;
        int on = nx_str_eq(val, "enable");
        feat_t *f = feat_find(line);
        if (f) f->enabled = (uint8_t)(on ? 1 : 0);
        else   feat_add(line, on);
    }
}

static void cfg_reload(void)
{
    static char buf[1024];
    int saved[1];
    if (!etc_enter(saved)) return;
    int sz = fs_size("features.conf");
    if (sz > 0) {
        if (sz > (int)sizeof buf - 1) sz = (int)sizeof buf - 1;
        for (int i = 0; i < (int)sizeof buf; i++) buf[i] = 0;
        fs_read("features.conf", (uint8_t *)buf, sizeof buf - 1);
        cfg_parse(buf);
    }
    etc_leave(saved);
}

/* write the whole table back; 0 = ok */
static int cfg_store(void)
{
    static char buf[1024];
    int saved[1];
    if (!etc_enter(saved)) return -1;
    int o = 0;
    for (int i = 0; i < nfeats; i++) {
        const char *st = feats[i].enabled ? "enable" : "disable";
        int nl = nx_str_len(feats[i].name);
        if (o + nl + 14 >= (int)sizeof buf) break;
        for (int k = 0; k < nl; k++) buf[o++] = feats[i].name[k];
        buf[o++] = '=';
        for (int k = 0; st[k]; k++) buf[o++] = st[k];
        buf[o++] = '\n';
    }
    buf[o] = 0;
    int r = fs_write("features.conf", (const uint8_t *)buf, (uint32_t)o);
    etc_leave(saved);
    return (r >= 0) ? 0 : -1;
}

/* ---------------- module hook stubs (requirement 6/8) ----------------
 * Shells around future logic: every hook's FIRST statement checks
 * feature_is_enabled() and refuses to run when the module is off. */

/* RTC via kernel.c: binary month/day of the current date */
extern void rtc_get_md(int *mon, int *day);
extern int  gfx_active(void);           /* gfx.c */
extern int  cn_can_render(const char *s);   /* cnfont.c */
extern int  cn_text16(int x, int y, const char *s, uint32_t rgb);

int feat_holiday_days_left(void)
{
    if (!feature_is_enabled("holiday_module")) return -1;   /* gated */
    int mon, day;
    rtc_get_md(&mon, &day);
    /* days until Oct 1 (national day); 0 on Oct 1..7 = holiday itself */
    int today = mon * 100 + day;
    if (today >= 1001 && today <= 1007) return 0;           /* golden week */
    int target = 1001;
    /* month lengths (non-leap; Feb close enough for a countdown) */
    static const int ml[13] = { 0,31,28,31,30,31,30,31,31,30,31,30,31 };
    int left = 0;
    if (today < 1001) {
        int m = mon, d = day;
        while (m != 10 || d != 1) {
            left++;
            d++;
            if (d > ml[m]) { d = 1; m++; if (m > 12) m = 1; }
        }
    } else {                          /* after golden week: next year's Oct 1 */
        int m = mon, d = day;
        for (;;) {
            left++;
            d++;
            if (d > ml[m]) { d = 1; m++; if (m > 12) m = 1; }
            if (m == 10 && d == 1) break;
        }
    }
    return left;
}

/* draw the egg banner in CJK: only when BOTH gates are on, the screen
 * is the LFB console, and every char is in the glyph bank */
void feat_holiday_banner(void)
{
    if (!feature_is_enabled("holiday_module")) return;      /* module gate */
    if (!feature_is_enabled("cn_lang_support")) return;     /* CJK gate */
    if (!gfx_active()) return;                              /* no LFB: skip */
    /* UTF-8: 国庆节快乐 */
    static const char msg[] =
        "\xE5\x9B\xBD\xE5\xBA\x86\xE8\x8A\x82\xE5\xBF\xAB\xE4\xB9\x90";
    if (!cn_can_render(msg)) return;
    cn_text16(472, 8, msg, 0x00FF5555u);                    /* festive red */
}

int feat_cn_toggle(void)
{
    if (!feature_is_enabled("cn_lang_support")) return -1;  /* gated */
    /* TODO cn_lang_support real logic (font bank switch, IME stub, ...) */
    return -2;
}

const char *feat_cn_greeting(void)
{
    if (!feature_is_enabled("cn_lang_support")) return 0;   /* gated */
    /* TODO localized greeting once CN glyphs exist */
    return "NovaOS (cn_lang_support skeleton)";
}

/* ---------------- public API ---------------- */

int feature_is_enabled(const char *name)
{
    if (!name) return 0;
    feat_t *f = feat_find(name);
    return f ? f->enabled : 0;
}

int feature_init(void)
{
    nfeats = 0;
    etc_dir = -1;
    /* seed the two built-in modules as enabled unless the file says otherwise */
    feat_add(builtin_names[0], 1);
    feat_add(builtin_names[1], 1);
    cfg_reload();
    /* make sure the file exists so users can see/edit it */
    int saved[1];
    if (etc_enter(saved)) {
        if (fs_size("features.conf") < 0) cfg_store();
        etc_leave(saved);
    }
    return 0;
}

int feature_list(char *buf, uint32_t max)
{
    if (!buf || max == 0) return nfeats;
    uint32_t o = 0;
    for (int i = 0; i < nfeats; i++) {
        const char *st = feats[i].enabled ? "enable" : "disable";
        int nl = nx_str_len(feats[i].name);
        if (o + (uint32_t)nl + 12 >= max) break;
        for (int k = 0; k < nl; k++) buf[o++] = feats[i].name[k];
        for (int k = nl; k < 21; k++) buf[o++] = ' ';      /* pad to col 21 */
        for (int k = 0; st[k]; k++) buf[o++] = st[k];
        buf[o++] = '\n';
    }
    if (max) buf[o < max ? o : max - 1] = 0;
    return nfeats;
}

int feature_set(const char *name, int enabled)
{
    feat_t *f = feat_find(name);
    if (!f) return -1;
    f->enabled = (uint8_t)(enabled ? 1 : 0);
    return cfg_store();     /* persist; parse on next boot restores it */
}
