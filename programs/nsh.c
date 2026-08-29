/* nsh.nxp - NovaSh subset shell + .nsh script engine, Ring 3.
 *
 * Build: build.ps1 compiles every programs\*.c (except nxp_entry.c)
 * and injects the result into the NovaFS template. Launch from the
 * kernel NovaSh with:   run nsh.nxp
 *
 * .nsh scripts are plain text files (create with NovaSh 'write') in a
 * .bat-compatible subset, executed here by API->readfile() + the SAME
 * command dispatcher the interactive prompt uses. Kept deliberately
 * tiny so the flat binary stays inside the 3072 B NovaFS limit:
 *
 *   @cmd               run one line without echoing it
 *   @echo off / on     stop / resume echoing script lines
 *   rem ... / :: ...   comments
 *   echo text          print text ('echo' alone prints ECHO is on)
 *   set N=V            variables, expanded as %N% (8 vars, name <= 11,
 *                      value <= 31 chars, unknown names stay literal)
 *   :label  goto L     labels + jumps (case sensitive, 'goto :L' ok)
 *   if [not] A==B cmd  string compare, no spaces in A/B (same quoting on
 *                      both sides cancels out)
 *   pause              wait for any key
 *   NAME.nsh           an unknown word is tried as a script (batch way)
 *   exit               leave the script / the shell
 *   help ver cls       plain commands
 *
 * (whoami/date/mem and colored output were cut for size - use the
 * kernel NovaSh. All buffers live in .bss, which the objcopy -j flags
 * drop from the flat binary - keep it that way or the 3072 B limit
 * will bite.) */
#include "nxp.h"

#define LINE_MAX 128
#define NVARS    8
#define NAMELEN  12
#define VALLEN   32
#define SCR_MAX  3072

static int str_eq(const char *a, const char *b)
{
    while (*a && *b) { if (*a != *b) return 0; a++; b++; }
    return *a == *b;
}
static int str_len(const char *s) { int n = 0; while (s[n]) n++; return n; }

static void p(char c)         { API->putc(c); }
static void ps(const char *s) { API->puts(s); }

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

static int  exec_line(char *line, int silent);
static int  run_script(const char *file);

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
            ps("no label\n");
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
static int exec_line(char *line, int silent)
{
    if (*line == '@') { silent = 1; line++; }
    while (*line == ' ') line++;
    if (!*line || *line == ':') return 0;          /* empty / label */
    if (!silent) { ps(line); p('\n'); }

    int i = 0;
    while (line[i] && line[i] != ' ') i++;
    char *args = line + i;
    if (*args) {
        *args = 0;
        args++;
        while (*args == ' ') args++;
    }
    char *cmd = line;

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
        char *eq2 = 0;
        for (char *q = args; *q; q++)
            if (*q == '=') { eq2 = q; break; }
        if (!eq2) return 0;                 /* bare set: do nothing */
        *eq2 = 0;
        char *nm = args, *vv = eq2 + 1;
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
        if (slot < 0) { ps("set: bad\n"); return 0; }
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
        if (!*args) return 0;
        for (int k = 0; args[k]; k++)
            if (args[k] == ' ') { args[k] = 0; break; }
        int al = str_len(args);
        if (al > 4 && str_eq(args + al - 4, ".nsh")) {
            if (in_script) { ps("no nesting\n"); return 0; }
            run_script(args);
            return 0;
        }
        int pid = API->spawn(args);                 /* .nxp: new process */
        if (pid < 0) { ps("spawn fail\n"); return 0; }
        ps("pid "); p('0' + (char)pid); p('\n');
        return 0;
    }
    if (str_eq(cmd, "procs")) {
        char pb[128];
        int n = API->procs(pb, (u32)sizeof pb);
        if (n <= 0) { ps("none\n"); return 0; }
        ps(pb);
        return 0;
    }
    if (str_eq(cmd, "exit")) {
        if (in_script) return 1;
        API->exit();
        return 1;
    }
    if (str_eq(cmd, "help")) {
        ps(" help echo X cls exit set N=V if goto run procs pause\n"
           " .nsh: @line rem :: echo on|off %N%\n");
        return 0;
    }
    if (str_eq(cmd, "cls")) { API->cls(); return 0; }

    /* batch habit: an unknown word may be a script file name */
    if (!in_script && run_script(cmd) == 0) return 0;

    ps("bad cmd: "); ps(cmd); p('\n');
    return 0;
}

void nxp_main(void)
{
    API->cls();
    ps("try 'hello.nsh'\n");

    char exp[LINE_MAX + VALLEN];
    for (;;) {
        int len = 0;
        char user[32];
        API->getuser(user, (u32)sizeof user);
        p('\n');
        ps(user);
        ps("@novaos:nsh# ");
        int cur = 0;                                /* cursor inside line */
        for (;;) {
            int c = API->getchar();
            if (c < 0) continue;                    /* non-blocking poll */
            if (c == NXP_KEY_LEFT) {                /* cursor left (no erase) */
                if (cur > 0) { cur--; p('\v'); }
            } else if (c == NXP_KEY_RIGHT) {        /* cursor right */
                if (cur < len) { p(line[cur]); cur++; }
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
        expand(line, exp);
        exec_line(exp, 1);          /* input loop already echoed the typing */
    }
}
