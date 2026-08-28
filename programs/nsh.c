/* nsh.nxp - NovaSh subset shell running unprivileged in Ring 3.
 *
 * Build: build.ps1 compiles every programs\*.c (except nxp_entry.c)
 * and injects the result into the NovaFS template. Launch it from
 * the kernel NovaSh with:   run nsh.nxp
 *
 * This is intentionally a *subset*: only commands that need no
 * filesystem access, no user management, and no nested .nxp
 * launching (those still live in the kernel NovaSh). Type 'exit'
 * to return to NovaSh. The point is to prove the shell can be
 * pulled out of the kernel into a user program. */
#include "nxp.h"

#define LINE_MAX 128

static int str_eq(const char *a, const char *b)
{
    while (*a && *b) { if (*a != *b) return 0; a++; b++; }
    return *a == *b;
}

static void p(char c)         { API->putc(c); }
static void ps(const char *s) { API->puts(s); }
static void col(u32 c)        { API->set_color(c); }

static void nsh_help(void)
{
    col(NXP_COLOR_DGRAY);
    ps(" --- nsh (ring3 subset) ---\n"
       " help    show this help\n"
       " ver     nsh version\n"
       " about   about nsh\n"
       " echo X  print text\n"
       " cls     clear screen\n"
       " whoami  current user\n"
       " date    date and time\n"
       " mem     memory layout\n"
       " exit    return to NovaSh\n");
    col(NXP_COLOR_WHITE);
}

static void nsh_ver(void)
{
    col(NXP_COLOR_LCYAN);
    ps("nsh v0.1 (ring3 subset) " __DATE__ "\n");
    col(NXP_COLOR_WHITE);
}

static void nsh_about(void)
{
    col(NXP_COLOR_LCYAN); ps("nsh"); col(NXP_COLOR_WHITE);
    ps(" - NovaSh subset shell, running unprivileged in Ring 3.\n"
       " fs / users / run still live in the kernel NovaSh.\n");
}

static void nsh_mem(void)
{
    ps("Memory map (static, hardcoded):\n"
       " Kernel image @ 0x00100000\n"
       " Stack top    @ 0x00200000\n"
       " NXP program  @ 0x00300000\n"
       " Trampoline   @ 0x00400000\n"
       " User stack   @ 0x00500000\n"
       " VGA buffer   @ 0x000B8000\n");
}

static void nsh_whoami(void)
{
    char buf[32];
    API->getuser(buf, (u32)sizeof buf);
    ps(buf); p('\n');
}

static void nsh_date(void)
{
    char buf[64];
    if (API->getdate(buf, (u32)sizeof buf) > 0) { ps(buf); p('\n'); }
}

static void prompt(void)
{
    char user[32];
    API->getuser(user, (u32)sizeof user);
    col(NXP_COLOR_DGRAY); p('\n');
    col(NXP_COLOR_LGREEN); ps(user);
    col(NXP_COLOR_LCYAN); ps("@novaos:");
    col(NXP_COLOR_YELLOW); ps("nsh");
    col(NXP_COLOR_DGRAY); p('#');
    col(NXP_COLOR_WHITE); p(' ');
}

void nxp_main(void)
{
    API->cls();
    col(NXP_COLOR_LCYAN); ps("nsh.nxp - NovaSh subset shell (Ring 3)\n");
    col(NXP_COLOR_DGRAY); ps("type 'help' for commands, 'exit' to return to NovaSh\n");
    col(NXP_COLOR_WHITE);

    char line[LINE_MAX];
    for (;;) {
        int len = 0;
        prompt();
        for (;;) {
            int c = API->getchar();
            if ((unsigned)c < 0x20 && c != '\n' && c != '\b') continue;
            if (c == '\n') { p('\n'); break; }
            if (c == '\b') { if (len > 0) { len--; p('\b'); } }
            else if (len < LINE_MAX - 1) { line[len++] = (char)c; p((char)c); }
        }
        line[len] = 0;

        /* split cmd / args on first space */
        int i = 0;
        while (line[i] == ' ') i++;
        int cstart = i;
        while (line[i] && line[i] != ' ') i++;
        char *args = 0;
        if (line[i] == ' ') {
            line[i] = 0;
            args = &line[i + 1];
            while (*args == ' ') args++;
        }
        char *cmd = &line[cstart];
        if (cmd[0] == 0) continue;

        if      (str_eq(cmd, "help"))   nsh_help();
        else if (str_eq(cmd, "ver"))    nsh_ver();
        else if (str_eq(cmd, "about"))  nsh_about();
        else if (str_eq(cmd, "echo"))   { if (args) ps(args); p('\n'); }
        else if (str_eq(cmd, "cls"))    API->cls();
        else if (str_eq(cmd, "whoami")) nsh_whoami();
        else if (str_eq(cmd, "date"))   nsh_date();
        else if (str_eq(cmd, "mem"))    nsh_mem();
        else if (str_eq(cmd, "exit"))   API->exit();
        else {
            col(NXP_COLOR_LRED);
            ps("Unknown command: "); ps(cmd); ps("  (try 'help')\n");
            col(NXP_COLOR_WHITE);
        }
    }
}
