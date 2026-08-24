/* ============================================================
 * NovaOS - minimal ACPI / AML support, just enough to power off.
 *
 * What it does:
 *   1. scan EBDA / 0xE0000-0xFFFFF for the RSDP (signature+checksum)
 *   2. walk RSDT (or XSDT on ACPI 2.0) entries to find the FADT
 *   3. read DSDT pointer + PM1a/PM1b control ports from the FADT
 *   4. run a tiny AML walker over DSDT and all SSDTs that only
 *      understands definition-level opcodes (Name/Scope/Method/
 *      Device/Processor/... ) and constant data, hunting for
 *      Name(\_S5, Package(SLP_TYPa, SLP_TYPb))
 *   5. acpi_poweroff(): outw(PM1a_CNT, (SLP_TYPa << 10) | SLP_EN)
 *
 * What it deliberately does NOT do: evaluate expressions, execute
 * methods, manage the namespace, SMI/ACPI-mode switching (QEMU has
 * the PM IO ports usable from reset).
 * ============================================================ */
#include "stdint.h"
#include "acpi.h"

/* ---- port I/O ---- */
static inline void outb_(uint16_t port, uint8_t v)
{
    __asm__ volatile ("outb %0, %1" : : "a"(v), "Nd"(port));
}
static inline void outw_(uint16_t port, uint16_t v)
{
    __asm__ volatile ("outw %0, %1" : : "a"(v), "Nd"(port));
}

/* identity-mapped physical memory reads */
#define P8(a)  (*(volatile uint8_t  *)(uint32_t)(a))
#define P16(a) (*(volatile uint16_t *)(uint32_t)(a))
#define P32(a) (*(volatile uint32_t *)(uint32_t)(a))

#define ACPI_SLP_EN      0x2000u   /* PM1_CNT bit 13 */
#define ACPI_AML_CLEAN   0xFFFFFFFFu

acpi_info_t g_acpi;

static uint32_t g_ssdt[8];
static int     g_ssdt_count;

/* ---- table checksums / signatures ---- */
static int sum_ok(uint32_t addr, uint32_t len)
{
    uint32_t s = 0;
    for (uint32_t i = 0; i < len; i++) s += P8(addr + i);
    return (uint8_t)s == 0;
}

static int sig4_eq(uint32_t addr, const char *sig)
{
    for (int i = 0; i < 4; i++)
        if ((char)P8(addr + i) != sig[i]) return 0;
    return 1;
}

static uint32_t find_rsdp(uint32_t base, uint32_t len)
{
    for (uint32_t a = base; a < base + len; a += 16) {
        if (P8(a)==0x52 && P8(a+1)==0x53 && P8(a+2)==0x44 && P8(a+3)==0x20 &&   /* "RSD " */
            P8(a+4)==0x50 && P8(a+5)==0x54 && P8(a+6)==0x52 && P8(a+7)==0x20 && /* "PTR " */
            sum_ok(a, 20))
            return a;
    }
    return 0;
}

/* find FADT + collect SSDTs from RSDT(32-bit entries)/XSDT(64-bit) */
static void scan_sdt(uint32_t sdt, int xsdt)
{
    uint32_t len = P32(sdt + 4);
    if (len < 36 || len > 0x10000 || !sum_ok(sdt, len)) return;
    uint32_t esz = xsdt ? 8 : 4;
    for (uint32_t off = 36; off + esz <= len; off += esz) {
        uint32_t t = P32(sdt + off);          /* low 32 bits; tables live < 4GB */
        if (t < 0x1000 || t >= 0xF0000000) continue;
        if (!g_acpi.fadt && sig4_eq(t, "FACP") &&
            P32(t + 4) >= 76 && sum_ok(t, P32(t + 4)))
            g_acpi.fadt = t;
        else if (sig4_eq(t, "SSDT") && g_ssdt_count < 8 &&
                 P32(t + 4) > 36 && sum_ok(t, P32(t + 4)))
            g_ssdt[g_ssdt_count++] = t;
    }
}

/* ============================================================
 * minimal AML walker
 * ============================================================ */
static const uint8_t *g_aml_base;   /* table start, for error offsets */

static void aml_fail(const uint8_t *at)
{
    if (g_acpi.aml_err == ACPI_AML_CLEAN)
        g_acpi.aml_err = (uint32_t)(at - g_aml_base);
}

/* PkgLength: lead byte bits[7:6]=extra byte count, bits[3:0]=low bits */
static int pkg_len(const uint8_t *p, const uint8_t *end,
                   uint32_t *plen, int *phdr)
{
    if (p >= end) return -1;
    uint8_t  b = *p;
    int      n = (b >> 6) & 3;
    uint32_t l;
    if (p + 1 + n > end) return -1;
    if (n == 0)
        l = b & 0x3F;              /* single byte: length in bits 5:0 */
    else {                         /* lead gives low 4 bits, bits 5:4 = 0 */
        l = b & 0x0F;
        for (int i = 0; i < n; i++) l |= (uint32_t)p[1 + i] << (4 + 8 * i);
    }
    if (l < (uint32_t)(1 + n)) return -1;     /* must cover its own header */
    *plen = l; *phdr = 1 + n;
    return 0;
}

static int seg_char(uint8_t c)
{
    return (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
}

/* NameString; reports the FINAL 4-char segment (seg may be NULL) */
static int parse_name(const uint8_t **pp, const uint8_t *end, char seg[4])
{
    const uint8_t *p = *pp, *s;
    while (p < end && (*p == 0x5C || *p == 0x5E)) p++;   /* \ ^ prefixes */
    if (p >= end) return -1;

    if (*p == 0x00) {                        /* NullName: root scope itself */
        if (seg) seg[0] = seg[1] = seg[2] = seg[3] = 0;
        *pp = p + 1;
        return 0;
    }
    if (*p == 0x2E) {                        /* dual name */
        s = ++p;
        if (s + 8 > end) return -1;
        for (int i = 0; i < 8; i++) if (!seg_char(s[i])) return -1;
        if (seg) { seg[0]=s[4]; seg[1]=s[5]; seg[2]=s[6]; seg[3]=s[7]; }
        p = s + 8;
    } else if (*p == 0x2F) {                 /* multi name */
        p++;
        if (p >= end) return -1;
        int n = *p++;
        if (n == 0 || p + (uint32_t)n * 4 > end) return -1;
        for (int i = 0; i < n * 4; i++) if (!seg_char(p[i])) return -1;
        if (seg) {
            int o = (n - 1) * 4;
            seg[0]=p[o]; seg[1]=p[o+1]; seg[2]=p[o+2]; seg[3]=p[o+3];
        }
        p += n * 4;
    } else {                                 /* single segment */
        s = p;
        if (s + 4 > end) return -1;
        for (int i = 0; i < 4; i++) if (!seg_char(s[i])) return -1;
        if (seg) { seg[0]=s[0]; seg[1]=s[1]; seg[2]=s[2]; seg[3]=s[3]; }
        p = s + 4;
    }
    *pp = p;
    return 0;
}

/* Byte/Word/DWord/QWord consts + Zero/One/Ones */
static int parse_const(const uint8_t **pp, const uint8_t *end, uint32_t *out)
{
    const uint8_t *p = *pp;
    uint32_t v = 0;
    if (p >= end) return -1;
    switch (*p++) {
    case 0x00: v = 0;          break;         /* ZeroOp */
    case 0x01: v = 1;          break;         /* OneOp */
    case 0xFF: v = 0xFFFFFFFF; break;         /* OnesOp */
    case 0x0A:
        if (p >= end) return -1;
        v = *p++; break;
    case 0x0B:
        if (p + 2 > end) return -1;
        v = p[0] | ((uint32_t)p[1] << 8); p += 2; break;
    case 0x0C:
        if (p + 4 > end) return -1;
        v = p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
        p += 4; break;
    case 0x0E:                                /* QWord: keep low 32 bits */
        if (p + 8 > end) return -1;
        v = p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
        p += 8; break;
    default:
        return -1;                            /* expressions unsupported */
    }
    *pp = p; *out = v;
    return 0;
}

/* skip a NameOp value: const / string / buffer / object reference */
static int skip_data(const uint8_t **pp, const uint8_t *end)
{
    const uint8_t *p = *pp;
    uint32_t len; int hd, is_ref;
    if (p >= end) return -1;

    if (*p == 0x11) {                         /* BufferOp */
        const uint8_t *start = p++;           /* PkgLength counts from its own
                                                lead byte, i.e. start+1 */
        if (pkg_len(p, end, &len, &hd) < 0 || start + 1 + len > end) return -1;
        p = start + 1 + len;
    } else if (*p == 0x0D) {                  /* String, NUL-terminated */
        p++;
        while (p < end && *p) p++;
        if (p >= end) return -1;
        p++;
    } else {
        is_ref = (*p == 0x5C || *p == 0x5E ||
                  (*p >= 'A' && *p <= 'Z') || *p == '_');
        if (is_ref)
            { if (parse_name(pp, end, 0) < 0) return -1; return 0; }
        if (parse_const(pp, end, &len) < 0) return -1;
        return 0;
    }
    *pp = p;
    return 0;
}

/* PackageOp: capture first two integer elements. Returns 0 if they
 * were readable, -1 if not (but *pp is still advanced past the
 * package whenever its structure itself was well formed). */
static int parse_package(const uint8_t **pp, const uint8_t *end, uint32_t v[2])
{
    const uint8_t *p = *pp, *start, *pend;
    uint32_t len, x; int hd, rc = -1;
    if (p >= end || *p != 0x12) return -1;
    start = p; p++;
    if (pkg_len(p, end, &len, &hd) < 0) return -1;
    if (start + 1 + len > end) return -1;     /* PkgLength counts from lead */
    pend = start + 1 + len; p += hd;
    if (p >= pend) return -1;
    p++;                                      /* NumElements byte */
    v[0] = v[1] = 0;
    if (parse_const(&p, pend, &x) == 0) { v[0] = x; rc = 0; }
    if (rc == 0 && p < pend && parse_const(&p, pend, &x) == 0) v[1] = x;
    *pp = pend;
    return rc;
}

/* walk definition-level terms between p and end */
static void walk_terms(const uint8_t *p, const uint8_t *end)
{
    while (p < end) {
        const uint8_t *s = p;                 /* term start, for PkgLength math */
        const uint8_t *tend;                  /* term end = pkglead + len */
        uint8_t op = *p++;
        uint32_t len; int hd;

        switch (op) {
        case 0x08: {                          /* NameOp name DataObject */
            char seg[4];
            if (parse_name(&p, end, seg) < 0) goto bad;
            if (p < end && *p == 0x12) {      /* Package value */
                uint32_t v[2];
                if (parse_package(&p, end, v) == 0 &&
                    seg[0]=='_' && seg[1]=='S' && seg[2]=='5' && seg[3]=='_' &&
                    !g_acpi.s5_found) {
                    g_acpi.s5_found = 1;
                    g_acpi.slp_typa = (uint16_t)(v[0] & 0x7);
                    g_acpi.slp_typb = (uint16_t)(v[1] & 0x7);
                }
            } else if (skip_data(&p, end) < 0) {
                goto bad;
            }
            break;
        }
        case 0x06:                            /* Alias name name */
            if (parse_name(&p, end, 0) < 0) goto bad;
            if (parse_name(&p, end, 0) < 0) goto bad;
            break;
        case 0x10: {                          /* Scope pkg name body */
            if (pkg_len(p, end, &len, &hd) < 0) goto bad;
            tend = p + len;                   /* p is at the PkgLength lead */
            if (tend > end) goto bad;
            p += hd;
            if (parse_name(&p, tend, 0) < 0) goto bad;
            walk_terms(p, tend);
            p = tend;
            break;
        }
        case 0x14:                            /* Method pkg ... : skip whole */
        case 0xA0:                            /* If pkg ... */
        case 0xA1:                            /* Else pkg ... */
        case 0xA2: {                          /* While pkg ... */
            if (pkg_len(p, end, &len, &hd) < 0) goto bad;
            tend = p + len;
            if (tend > end) goto bad;
            p = tend;
            break;
        }
        case 0x00:                            /* stray ZeroOp as statement */
        case 0x01:                            /* OneOp */
        case 0xFF:                            /* OnesOp */
        case 0xA3:                            /* NoOp */
        case 0xA5:                            /* Break */
        case 0xCC:                            /* BreakPoint */
            break;                            /* harmless at definition level */
        case 0x5B: {                          /* extended opcodes */
            if (p >= end) goto bad;
            uint8_t sub = *p++;               /* pkg lead now at p = s+2 */
            switch (sub) {
            case 0x82:                        /* Device pkg name body */
            case 0x84: {                      /* ThermalZone pkg name body */
                if (pkg_len(p, end, &len, &hd) < 0) goto bad;
                tend = p + len;
                if (tend > end) goto bad;
                p += hd;
                if (parse_name(&p, tend, 0) < 0) goto bad;
                walk_terms(p, tend);
                p = tend;
                break;
            }
            case 0x83: {                      /* Processor pkg name +5 body */
                if (pkg_len(p, end, &len, &hd) < 0) goto bad;
                tend = p + len;
                if (tend > end) goto bad;
                p += hd;
                if (parse_name(&p, tend, 0) < 0 || p + 5 > tend) goto bad;
                walk_terms(p + 5, tend);
                p = tend;
                break;
            }
            case 0x85: {                      /* PowerResource pkg name +2 body */
                if (pkg_len(p, end, &len, &hd) < 0) goto bad;
                tend = p + len;
                if (tend > end) goto bad;
                p += hd;
                if (parse_name(&p, tend, 0) < 0 || p + 2 > tend) goto bad;
                walk_terms(p + 2, tend);
                p = tend;
                break;
            }
            case 0x81:                        /* Field pkg ... */
            case 0x86:                        /* IndexField pkg ... */
            case 0x87: {                      /* BankField pkg ... */
                if (pkg_len(p, end, &len, &hd) < 0) goto bad;
                tend = p + len;
                if (tend > end) goto bad;
                p = tend;
                break;
            }
            case 0x80: {                      /* OperationRegion name space off len */
                if (parse_name(&p, end, 0) < 0 || p >= end) goto bad;
                p++;                          /* RegionSpace */
                if (parse_const(&p, end, &len) < 0) goto bad;   /* offset */
                if (parse_const(&p, end, &len) < 0) goto bad;   /* length */
                break;
            }
            case 0x88: {                      /* DataRegion name + 3 termargs */
                if (parse_name(&p, end, 0) < 0) goto bad;
                for (int i = 0; i < 3; i++)
                    if (skip_data(&p, end) < 0) goto bad;
                break;
            }
            case 0x01:                        /* Mutex name u8 */
                if (parse_name(&p, end, 0) < 0 || p + 1 > end) goto bad;
                p++;
                break;
            case 0x02:                        /* Event name */
                if (parse_name(&p, end, 0) < 0) goto bad;
                break;
            default:
                goto bad;
            }
            break;
        }
        default:
            goto bad;                         /* unknown term: give up here */
        }
        continue;
    bad:
        aml_fail(s);
        return;                               /* parents resume at their bounds */
    }
}

static void walk_aml_table(uint32_t t)
{
    uint32_t len = P32(t + 4);
    if (len <= 36 || len > 0x100000) return;
    g_aml_base = (const uint8_t *)t;
    walk_terms((const uint8_t *)(t + 36), (const uint8_t *)(t + len));
}

/* ============================================================
 * public API
 * ============================================================ */
int acpi_init(void)
{
    uint32_t rsdp = 0, ebda, rsdt, xsdt;

    g_acpi.aml_err = ACPI_AML_CLEAN;
    g_ssdt_count = 0;

    /* RSDP lives in the EBDA or the BIOS area below 1MB */
    ebda = (uint32_t)P16(0x40E) << 4;
    if (ebda >= 0x80000 && ebda < 0xA0000) rsdp = find_rsdp(ebda, 0x400);
    if (!rsdp) rsdp = find_rsdp(0xE0000, 0x20000);
    if (!rsdp) return -1;
    g_acpi.rsdp = rsdp;

    rsdt = P32(rsdp + 16);
    xsdt = (P8(rsdp + 15) >= 2) ? P32(rsdp + 24) : 0;   /* low 32 of u64 */
    if (xsdt)                { g_acpi.xsdt = 1; g_acpi.sdt = xsdt; }
    else if (rsdt)           { g_acpi.xsdt = 0; g_acpi.sdt = rsdt; }
    else return -2;
    scan_sdt(g_acpi.sdt, g_acpi.xsdt);

    if (!g_acpi.fadt) return -3;
    g_acpi.dsdt     = P32(g_acpi.fadt + 40);
    g_acpi.pm1a_cnt = P32(g_acpi.fadt + 64);
    g_acpi.pm1b_cnt = P32(g_acpi.fadt + 68);

    /* hunt \_S5 in the DSDT, then any SSDTs */
    if (g_acpi.dsdt && sig4_eq(g_acpi.dsdt, "DSDT"))
        walk_aml_table(g_acpi.dsdt);
    for (int i = 0; i < g_ssdt_count && !g_acpi.s5_found; i++)
        walk_aml_table(g_ssdt[i]);

    return 0;
}

void acpi_poweroff(void)
{
    uint16_t typa = g_acpi.s5_found ? g_acpi.slp_typa : 0;
    uint16_t typb = g_acpi.s5_found ? g_acpi.slp_typb : 0;
    if (g_acpi.pm1a_cnt) outw_((uint16_t)g_acpi.pm1a_cnt,
                               (uint16_t)((typa << 10) | ACPI_SLP_EN));
    if (g_acpi.pm1b_cnt) outw_((uint16_t)g_acpi.pm1b_cnt,
                               (uint16_t)((typb << 10) | ACPI_SLP_EN));
}
