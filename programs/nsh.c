/* nsh.nxp - the FULL NovaSh, running as a process in Ring 3.
 *
 * Build: build.ps1 compiles every programs\*.c (except nxp_entry.c)
 * and injects the result into the NovaFS template (files up to ~67 KB
 * via the indirect-block scheme). Launch from the kernel NovaSh with:
 *   run nsh.nxp
 *
 * The complete command set of the kernel shell is available here:
 *   help ver about echo X cls date whoami mem exit logout
 *   ls cd mkdir rmdir rd rm cat write format fsinfo mkdemo
 *   useradd userdel su passwd acpi reboot shutdown halt
 *   run F.nsh/.nxp  procs fg kill N  pause
 * Filesystem / user / power commands are syscalls (listdir/fsop/sysop)
 * that reuse the kernel's own command implementations; file ownership
 * and the process cwd apply exactly as in the kernel shell.
 *
 * Plus the .bat-compatible .nsh script engine (identical syntax to the
 * kernel-side engine in kernel.c):
 *   @cmd / @echo off|on   rem / ::    echo text    set N=V, use as %N%
 *   :label / goto L       if [not] A==B cmd       pause      exit
 * Typing NAME.nsh runs it; `run F.nxp` spawns a new process.
 *
 * Everything big lives in .bss, which objcopy -j drops from the flat
 * binary. */
#include "nxp.h"

#define LINE_MAX 128
#define NVARS    8
#define NAMELEN  12
#define VALLEN   32
#define SCR_MAX  16384

static int str_eq(const char *a, const char *b)
{
    while (*a && *b) { if (*a != *b) return 0; a++; b++; }
    return *a == *b;
}
static int str_len(const char *s) { int n = 0; while (s[n]) n++; return n; }

static void p(char c)         { API->putc(c); }
static void ps(const char *s) { API->puts(s); }

static void pdec(u32 v)
{
    char t[12]; int k = 0;
    if (!v) t[k++] = '0';
    while (v) { t[k++] = (char)('0' + v % 10); v /= 10; }
    while (k--) p(t[k]);
}

/* ---- batch variables (bss, zeroed by the kernel loader) ---- */
static char vname[NVARS][NAMELEN];
static char vval[NVARS][VALLEN];
static int  echo_on = 1;        /* echo executed script lines */
static int  in_script = 0;
static int  goto_flag = 0;
static char goto_target[24];

static char *var_get(const char *name, int len)
{
    for (int i = 0; i < NVARS; i++) {
        if (!vname[i][0] || str_len(vname[i]) != len) continue;
        int j = 0;
        while (j < len && vname[i][j] == name[j]) j++;
        if (j == len) return vval[i];
    }
    return 0;
}

/* %N% expansion into out (out must hold LINE_MAX+VALLEN bytes) */
static void expand(const char *in, char *out)
{
    int o = 0;
    for (int i = 0; in[i] && o < LINE_MAX + VALLEN - 2; i++) {
        if (in[i] != '%') { out[o++] = in[i]; continue; }
        int j = i + 1;
        while (in[j] && in[j] != '%') j++;
        if (!in[j]) { out[o++] = '%'; continue; }        /* unmatched % */
        char *v = var_get(in + i + 1, j - i - 1);
        if (!v) { out[o++] = '%'; continue; }            /* unknown var */
        while (*v && o < LINE_MAX + VALLEN - 2) out[o++] = *v++;
        i = j;
    }
    out[o] = 0;
}

static char sbuf[SCR_MAX];    /* script text buffer */
static char line[LINE_MAX];   /* interactive input line */

/* redraw the tail after an edit, park the cursor back at cur */
static void retail(int cur, int len)
{
    for (int i = cur; i < len; i++) p(line[i]);
    for (int i = cur; i < len; i++) p('\v');
}

static int  exec_line(char *l, int silent);
static int  run_script(const char *file);   /* 0 = ran, -1 = unreadable */

static int run_script(const char *file)
{
    int n = API->readfile(file, (unsigned char *)sbuf, SCR_MAX - 1);
    if (n <= 0) return -1;
    sbuf[n] = 0;
    in_script = 1;
    char *lp = sbuf;
    while (*lp) {
        char raw[LINE_MAX];
        char exp[LINE_MAX + VALLEN];
        int rl = 0;
        char *e = lp;
        while (*e && *e != '\n') {
            if (*e != '\r' && rl < LINE_MAX - 1) raw[rl++] = *e;
            e++;
        }
        raw[rl] = 0;
        expand(raw, exp);
        int stop = exec_line(exp, !echo_on);
        if (goto_flag) {
            char *q = sbuf;
            int found = 0;
            while (*q && !found) {
                char *e2 = q;
                while (*e2 && *e2 != '\n') e2++;
                char sv = *e2;
                *e2 = 0;
                char *t = q;
                while (*t == ' ') t++;
                if (*t == ':') {
                    t++;
                    int m = 0;
                    while (goto_target[m] && t[m] == goto_target[m]) m++;
                    if (!goto_target[m] && (t[m] == 0 || t[m] == ' ')) found = 1;
                }
                *e2 = sv;
                if (!found) q = sv ? e2 + 1 : e2;
            }
            if (found) { lp = q; goto_flag = 0; continue; }
            ps("goto: no label\n");
            break;
        }
        if (stop) break;
        lp = *e ? e + 1 : e;
    }
    in_script = 0;
    goto_flag = 0;
    return 0;
}

/* one command line, interactive or script; returns 1 = stop script */
static int exec_line(char *l, int silent)
{
    if (*l == '@') { silent = 1; l++; }
    while (*l == ' ') l++;
    if (!*l || *l == ':') return 0;          /* empty / label */
    if (!silent) { ps(l); p('\n'); }

    int i = 0;
    while (l[i] && l[i] != ' ') i++;
    char *args = l + i;
    if (*args) {
        *args = 0;
        args++;
        while (*args == ' ') args++;
    }
    char *cmd = l;

    if (str_eq(cmd, "rem")) return 0;
    if (str_eq(cmd, "echo")) {
        if (!*args) { p('\n'); return 0; }
        if (str_eq(args, "off")) { echo_on = 0; return 0; }
        if (str_eq(args, "on"))  { echo_on = 1; return 0; }
        ps(args); p('\n');
        return 0;
    }
    if (str_eq(cmd, "pause")) {
        ps("Press a key\n");
        API->getchar();
        return 0;
    }
    if (str_eq(cmd, "set")) {
        char *eq = 0;
        for (char *q = args; *q; q++) if (*q == '=') { eq = q; break; }
        if (!eq) { ps("set N=V\n"); return 0; }
        *eq = 0;
        char *nm = args, *vv = eq + 1;
        while (*vv == ' ') vv++;
        int nl = str_len(nm);
        while (nl && nm[nl - 1] == ' ') nm[--nl] = 0;
        int vl = str_len(vv);
        if (!nl || nl >= NAMELEN || vl >= VALLEN) { ps("set: bad\n"); return 0; }
        int slot = -1;
        for (int k = 0; k < NVARS && slot < 0; k++)
            if (vname[k][0] && str_eq(vname[k], nm)) slot = k;
        if (slot < 0)
            for (int k = 0; k < NVARS && slot < 0; k++)
                if (!vname[k][0]) slot = k;
        if (slot < 0) { ps("set: full\n"); return 0; }
        for (int k = 0; k < nl; k++) vname[slot][k] = nm[k];
        vname[slot][nl] = 0;
        for (int k = 0; k < vl; k++) vval[slot][k] = vv[k];
        vval[slot][vl] = 0;
        return 0;
    }
    if (str_eq(cmd, "goto")) {
        if (!in_script) { ps("script only\n"); return 0; }
        if (*args == ':') args++;
        int m = 0;
        while (args[m] && args[m] != ' ' && m < 23) {
            goto_target[m] = args[m];
            m++;
        }
        goto_target[m] = 0;
        if (m) goto_flag = 1;
        return 0;
    }
    if (str_eq(cmd, "if")) {
        if (!in_script) { ps("script only\n"); return 0; }
        char *s = args;
        int invert = 0;
        if (s[0] == 'n' && s[1] == 'o' && s[2] == 't' && (s[3] == ' ' || s[3] == 0)) {
            invert = 1;
            s += 3;
            while (*s == ' ') s++;
        }
        char *eq = 0;
        for (char *q = s; q[0] && q[1]; q++)
            if (q[0] == '=' && q[1] == '=') { eq = q; break; }
        if (!eq) { ps("need ==\n"); return 0; }
        *eq = 0;
        char *L = s, *R = eq + 2;
        while (*R == ' ') R++;
        char *rest = R;
        while (*rest && *rest != ' ') rest++;
        if (*rest) { *rest = 0; rest++; while (*rest == ' ') rest++; }
        int c = str_eq(L, R);
        if (invert) c = !c;
        if (c && *rest) return exec_line(rest, 1);
        return 0;
    }
    if (str_eq(cmd, "run")) {
        if (!*args) { ps("run F.nsh / F.nxp\n"); return 0; }
        for (int k = 0; args[k]; k++)
            if (args[k] == ' ') { args[k] = 0; break; }
        int al = str_len(args);
        if (al > 4 && str_eq(args + al - 4, ".nsh")) {
            if (in_script) { ps("no nesting\n"); return 0; }
            if (run_script(args) < 0) ps("cannot read it\n");
            return 0;
        }
        int pid = API->spawn(args);                 /* .nxp: new process */
        if (pid < 0) { ps("spawn fail\n"); return 0; }
        ps("pid "); pdec((u32)pid); p('\n');
        return 0;
    }
    if (str_eq(cmd, "procs")) {
        char pb[128];
        int n = API->procs(pb, (u32)sizeof pb);
        if (n <= 0) { ps("none\n"); return 0; }
        ps(pb);
        return 0;
    }

    /* ---- filesystem commands (kernel implementations via fsop) ---- */
    if (str_eq(cmd, "ls")) {
        char lb[1024];
        int n = API->listdir(lb, (u32)sizeof lb);
        if (n < 0) { ps("ls failed\n"); return 0; }
        if (n == 0) ps("  (empty)\n");
        else ps(lb);
        ps("  "); pdec((u32)n); ps(" item(s)\n");
        return 0;
    }
    if (str_eq(cmd, "cd"))    { API->fsop(5, args); return 0; }
    if (str_eq(cmd, "mkdir")) { if (!*args) { ps("mkdir D\n"); return 0; }
                                API->fsop(1, args); return 0; }
    if (str_eq(cmd, "rmdir")) { if (!*args) { ps("rmdir D\n"); return 0; }
                                API->fsop(2, args); return 0; }
    if (str_eq(cmd, "rd"))    { if (!*args) { ps("rd D\n"); return 0; }
                                API->fsop(3, args); return 0; }
    if (str_eq(cmd, "rm"))    { if (!*args) { ps("rm F\n"); return 0; }
                                API->fsop(4, args); return 0; }
    if (str_eq(cmd, "format")) { API->fsop(6, ""); return 0; }
    if (str_eq(cmd, "fsinfo")) { API->fsop(7, ""); return 0; }
    if (str_eq(cmd, "cat")) {
        if (!*args) { ps("cat F\n"); return 0; }
        static char cb[3072];
        int n = API->readfile(args, (unsigned char *)cb, (u32)sizeof cb);
        if (n < 0) { ps("no such file\n"); return 0; }
        for (int k = 0; k < n; k++) p(cb[k]);
        if (n > 0 && cb[n - 1] != '\n') p('\n');
        return 0;
    }
    if (str_eq(cmd, "write")) {
        if (!*args) { ps("write F\n"); return 0; }
        ps("End with a single '.'\n");
        static unsigned char wb[8192];
        int wl = 0;
        for (;;) {
            char lb[LINE_MAX];
            ps("> ");
            int ll = 0;
            for (;;) {
                int c = API->getchar();
                if (c < 0) continue;
                if ((unsigned)c < 0x20 && c != '\n' && c != '\b') continue;
                if (c == '\n') { p('\n'); break; }
                if (c == '\b') { if (ll > 0) { ll--; p('\b'); } }
                else if (ll < LINE_MAX - 1) { lb[ll++] = (char)c; p((char)c); }
            }
            lb[ll] = 0;
            if (ll == 1 && lb[0] == '.') break;
            for (int k = 0; lb[k] && wl < (int)sizeof wb - 1; k++) wb[wl++] = lb[k];
            if (wl < (int)sizeof wb - 1) wb[wl++] = '\n';
        }
        int r = API->writefile(args, wb, (u32)wl);
        if (r < 0) { ps("write failed (permission?)\n"); return 0; }
        ps("Saved "); pdec((u32)r); ps(" bytes\n");
        return 0;
    }

    /* ---- users / system (kernel implementations via sysop) ---- */
    if (str_eq(cmd, "useradd"))  { if (!*args) { ps("useradd U\n"); return 0; }
                                   API->sysop(1, args); return 0; }
    if (str_eq(cmd, "userdel"))  { if (!*args) { ps("userdel U\n"); return 0; }
                                   API->sysop(2, args); return 0; }
    if (str_eq(cmd, "passwd"))   { API->sysop(3, ""); return 0; }
    if (str_eq(cmd, "su"))       { if (!*args) { ps("su U\n"); return 0; }
                                   API->sysop(4, args); return 0; }
    if (str_eq(cmd, "mkdemo"))   { API->sysop(5, ""); return 0; }
    if (str_eq(cmd, "acpi"))     { API->sysop(6, ""); return 0; }
    if (str_eq(cmd, "reboot"))   { API->sysop(7, ""); return 0; }
    if (str_eq(cmd, "shutdown")) { API->sysop(8, ""); return 0; }
    if (str_eq(cmd, "halt"))     { API->sysop(9, ""); return 0; }
    if (str_eq(cmd, "fg"))       { API->sysop(10, ""); ps("resumed\n"); return 0; }
    if (str_eq(cmd, "kill"))     { if (!*args) { ps("kill N\n"); return 0; }
                                   API->sysop(11, args); return 0; }
    if (str_eq(cmd, "netinfo"))  { API->sysop(12, ""); return 0; }
    if (str_eq(cmd, "ping"))     { if (!*args) { ps("ping IP\n"); return 0; }
                                   API->sysop(13, args); return 0; }
    if (str_eq(cmd, "dhcp"))     { API->sysop(14, ""); return 0; }
    if (str_eq(cmd, "dns"))      { if (!*args) { ps("dns NAME\n"); return 0; }
                                   API->sysop(15, args); return 0; }
    if (str_eq(cmd, "wget"))     { if (!*args) { ps("wget HOST[:PORT] [/PATH]\n"); return 0; }
                                   API->sysop(17, args); return 0; }
    if (str_eq(cmd, "udpecho"))  { API->sysop(16, ""); return 0; }
    /* feature / feture (alias): module switches via sysop 19/20 */
    if (str_eq(cmd, "feature") || str_eq(cmd, "feture")) {
        if (!*args) {
            char lb[512];
            API->sysop(19, lb);
            ps(lb);
            return 0;
        }
        char sub[12], name[24];
        int k = 0;
        char *s2 = args;
        while (*s2 == ' ') s2++;
        while (*s2 && *s2 != ' ' && k < 11) sub[k++] = *s2++;
        sub[k] = 0;
        while (*s2 == ' ') s2++;
        k = 0;
        while (*s2 && *s2 != ' ' && k < 23) name[k++] = *s2++;
        name[k] = 0;
        if (!name[0] || (str_eq(sub, "enable") == 0 && str_eq(sub, "disable") == 0)) {
            ps("usage: feature [enable NAME | disable NAME]\n");
            return 0;
        }
        char arg[40];
        k = 0;
        for (int j = 0; name[j]; j++) arg[k++] = name[j];
        arg[k++] = '=';
        for (int j = 0; sub[j]; j++) arg[k++] = sub[j];
        arg[k] = 0;
        if (API->sysop(20, arg) < 0) ps("feature: no such module\n");
        return 0;
    }
    if (str_eq(cmd, "logout"))   { API->exit(); return 0; }

    /* ---- local info commands ---- */
    if (str_eq(cmd, "date") || str_eq(cmd, "time")) {
        char db[64];
        if (API->getdate(db, (u32)sizeof db) > 0) ps(db);
        return 0;
    }
    if (str_eq(cmd, "whoami")) {
        char ub[32];
        API->getuser(ub, (u32)sizeof ub);
        ps(ub); p('\n');
        return 0;
    }
    if (str_eq(cmd, "ver"))   { ps("nsh v1.1 - full NovaSh in Ring3\n"); return 0; }
    if (str_eq(cmd, "about")) {
        ps("NovaOS - a tiny 32-bit OS with preemptive multitasking\n");
        return 0;
    }
    if (str_eq(cmd, "mem")) {
        ps("Kernel@0x100000  Slots@0x300000  Tramp@0x400000  Stack@0x500000\n");
        return 0;
    }
    if (str_eq(cmd, "cls")) { API->cls(); return 0; }
    if (str_eq(cmd, "exit")) {
        if (in_script) return 1;
        API->exit();
        return 1;
    }
    if (str_eq(cmd, "help")) {
        ps(" help ver about echo X cls date whoami mem exit logout\n"
           " ls cd mkdir rmdir rd rm cat write format fsinfo mkdemo\n"
           " useradd userdel su passwd acpi reboot shutdown halt\n"
           " netinfo dhcp ping IP dns NAME wget H[/PATH] udpecho\n"
           " run F.nsh/.nxp  procs fg kill N  pause  set N=V  if goto\n"
           " .nsh: @line rem :: echo on|off %N%\n");
        return 0;
    }

    /* batch habit: an unknown word may be a script file name */
    if (!in_script && run_script(cmd) == 0) return 0;

    ps("bad cmd: "); ps(cmd); p('\n');
    return 0;
}

/* command history (↑/↓ recall, same behavior as the kernel NovaSh) */
#define HIST_N 16
#define HIST_NAV_LIVE (-1)
static char hist[HIST_N][LINE_MAX];
static int  hist_count, hist_head, hist_nav = HIST_NAV_LIVE;
static char hist_draft[LINE_MAX];

static void hist_push(const char *l)
{
    if (!l[0]) return;
    if (hist_count && str_eq(hist[hist_head], l)) return;
    hist_head = (hist_head + 1) % HIST_N;
    int i = 0;
    for (; l[i] && i < LINE_MAX - 1; i++) hist[hist_head][i] = l[i];
    hist[hist_head][i] = 0;
    if (hist_count < HIST_N) hist_count++;
}

static void hist_load(int nav, char *l)
{
    int idx = (hist_head - nav + HIST_N) % HIST_N;
    int i = 0;
    for (; hist[idx][i] && i < LINE_MAX - 1; i++) l[i] = hist[idx][i];
    l[i] = 0;
}

/* ---- tab completion (same rules as the kernel NovaSh) ---- */
static const char * const cmd_tab[] = {
    "help","ver","about","echo","cls","date","time","whoami","mem","exit",
    "logout","ls","cd","mkdir","rmdir","rd","rm","cat","write","format",
    "fsinfo","mkdemo","useradd","userdel","su","passwd","acpi","reboot",
    "shutdown","halt","netinfo","dhcp","ping","dns","wget","udpecho",
    "feature","feture","run","procs","fg","kill","pause","set","if","goto",
    "rem",0
};

#define TAB_MATCH 64
static char tab_names[TAB_MATCH][26];
static int  tab_isdir[TAB_MATCH];
static int  tab_n;

static int tab_collect_cb(const char *name, int type, u32 size)
{
    (void)size;
    if (tab_n < TAB_MATCH) {
        int i = 0;
        while (name[i] && i < 25) { tab_names[tab_n][i] = name[i]; i++; }
        tab_names[tab_n][i] = 0;
        tab_isdir[tab_n] = (type == 2);         /* T_DIR */
        tab_n++;
    }
    return 0;
}

static int tab_complete(char *line, int *lenp, int *curp)
{
    int start = *curp;
    while (start > 0 && line[start - 1] != ' ') start--;
    int wlen = *curp - start;
    if (wlen >= 24) return 0;

    const char * const *cmds = 0;
    if (start == 0) cmds = cmd_tab;
    else {
        tab_n = 0;
        static char lb[2048];                   /* stack: syscall buffer */
        char *sp = lb;
        API->listdir(sp, (u32)sizeof lb);
        /* reuse the shell's own ls parse: walk lines, pull names */
        const char *q = sp;
        while (*q && tab_n < TAB_MATCH) {
            if (q[0] == ' ' && q[1] == ' ' && q[2] == '[' &&
                (q[3] == 'D' || q[3] == 'F')) {
                int isd = (q[3] == 'D');
                const char *s = q + 16;         /* tag(9) + mode(6) + space(1) */
                int k = 0;
                while (*s && *s != '\n' && *s != '(' && k < 25)
                    { tab_names[tab_n][k++] = *s++; }
                while (k > 0 && tab_names[tab_n][k-1] == ' ') k--;
                tab_names[tab_n][k] = 0;
                tab_isdir[tab_n] = isd;
                tab_n++;
            }
            while (*q && *q != '\n') q++;
            if (*q) q++;
        }
    }

    int nm = 0, lcp = 26;
    for (int i = 0; ; i++) {
        const char *c = cmds ? cmds[i] : (i < tab_n ? tab_names[i] : 0);
        if (!c) break;
        int k = 0;
        while (k < wlen && c[k] && c[k] == line[start + k]) k++;
        if (k < wlen) continue;
        nm++;
        int cl = 0;
        while (c[cl]) cl++;
        if (cl < lcp) lcp = cl;
    }
    if (nm == 0) return 0;

    char ins[28];
    int add = 0;
    if (nm == 1) {
        const char *c = 0;
        int mi = -1;
        if (cmds) {
            for (int i = 0; cmd_tab[i]; i++) {
                int k = 0;
                while (k < wlen && cmd_tab[i][k] && cmd_tab[i][k] == line[start + k]) k++;
                if (k < wlen) continue;
                c = cmd_tab[i];
                break;
            }
        } else {
            for (int i = 0; i < tab_n; i++) {
                int k = 0;
                while (k < wlen && tab_names[i][k] && tab_names[i][k] == line[start + k]) k++;
                if (k < wlen) continue;
                c = tab_names[i];
                mi = i;
                break;
            }
        }
        if (!c) return 0;
        int cl = 0;
        while (c[cl]) cl++;
        for (int q = wlen; q < cl && add < 26; q++) ins[add++] = c[q];
        ins[add++] = (!cmds && mi >= 0 && tab_isdir[mi]) ? '/' : ' ';
    } else {
        int best = wlen;
        for (int q = wlen; q < lcp; q++) {
            char ref = 0;
            int seen = 0, same = 1;
            for (int i = 0; ; i++) {
                const char *c = cmds ? cmds[i] : (i < tab_n ? tab_names[i] : 0);
                if (!c) break;
                if (!c[q]) { same = 0; break; }
                if (!seen) { ref = c[q]; seen = 1; }
                else if (c[q] != ref) { same = 0; break; }
            }
            if (!same || !ref) break;
            best = q + 1;
        }
        if (best <= wlen) return 0;
        /* copy the shared prefix from the first match */
        for (int i = 0; ; i++) {
            const char *c = cmds ? cmds[i] : (i < tab_n ? tab_names[i] : 0);
            if (!c) break;
            for (int q = wlen; q < best && add < 26; q++) ins[add++] = c[q];
            break;
        }
    }
    if (add <= 0 || *lenp + add > LINE_MAX - 1) return 0;
    for (int i = *lenp; i >= *curp; i--) line[i + add] = line[i];
    for (int i = 0; i < add; i++) line[*curp + i] = ins[i];
    *lenp += add;
    for (int i = 0; i < add; i++) p(ins[i]);
    *curp += add;
    retail(*curp, *lenp);
    return add;
}

void nxp_main(void)
{
    API->cls();
    ps("nsh v1.2 - full NovaSh in Ring 3\n");

    char exp[LINE_MAX + VALLEN];
    for (;;) {
        int len = 0;
        int cur = 0;                                /* cursor inside line */
        char user[32];
        hist_nav = HIST_NAV_LIVE;
        API->getuser(user, (u32)sizeof user);
        p('\n');
        ps(user);
        ps("@novaos:nsh# ");
        for (;;) {
            int c = API->getchar();
            if (c < 0) continue;                    /* non-blocking poll */
            if (c == NXP_KEY_LEFT) {                /* cursor left (no erase) */
                if (cur > 0) { cur--; p('\v'); }
            } else if (c == NXP_KEY_RIGHT) {        /* cursor right */
                if (cur < len) { p(line[cur]); cur++; }
            } else if (c == NXP_KEY_UP || c == NXP_KEY_DOWN) {   /* history */
                int want = hist_nav + ((c == NXP_KEY_UP) ? 1 : -1);
                char buf[LINE_MAX];
                int ok = 0;
                if (c == NXP_KEY_UP && want < hist_count) {
                    if (hist_nav == HIST_NAV_LIVE) {
                        for (int i = 0; i < len; i++) hist_draft[i] = line[i];
                        hist_draft[len] = 0;
                    }
                    hist_load(want, buf);
                    hist_nav = want;
                    ok = 1;
                } else if (c == NXP_KEY_DOWN && hist_nav > HIST_NAV_LIVE) {
                    hist_nav--;
                    if (hist_nav == HIST_NAV_LIVE) {
                        int i = 0;
                        while (hist_draft[i] && i < LINE_MAX - 1) { buf[i] = hist_draft[i]; i++; }
                        buf[i] = 0;
                    } else hist_load(hist_nav, buf);
                    ok = 1;
                }
                if (ok) {
                    for (int i = 0; i < len; i++) p('\v');
                    int nl = 0;
                    while (buf[nl]) nl++;
                    for (int i = 0; i < nl; i++) p(buf[i]);
                    for (int i = nl; i < len; i++) p(' ');
                    for (int i = nl; i < len; i++) p('\v');
                    for (int i = 0; i < nl; i++) line[i] = buf[i];
                    line[nl] = 0;
                    len = nl;
                    cur = nl;
                }
            } else if (c == '\t') {                 /* tab completion */
                tab_complete(line, &len, &cur);
            } else if (c == '\n') { p('\n'); break; }
            else if (c == '\b') {                   /* delete before cursor */
                if (cur > 0) {
                    for (int i = cur - 1; i < len - 1; i++) line[i] = line[i + 1];
                    len--; cur--; p('\b');
                    retail(cur, len);
                }
            } else if ((unsigned)c < 0x20) continue;                /* other control keys */
            else if (len < LINE_MAX - 1) {          /* insert at the cursor */
                for (int i = len; i > cur; i--) line[i] = line[i - 1];
                line[cur++] = (char)c; len++; p((char)c);
                retail(cur, len);
            }
        }
        line[len] = 0;
        hist_push(line);
        expand(line, exp);
        exec_line(exp, 1);          /* input loop already echoed the typing */
    }
}
