/* ps3recomp lifter torture suite -- framework + main.
 *
 * All reporting I/O uses raw `sc` (sys_tty_write, syscall 403) and manual
 * hex formatting: only shifts, masks, and byte stores. If even THOSE are
 * mis-lifted, everything fails loudly, which is still a signal.
 */
#include "torture.h"

/* ------------------------------------------------------------------ */
/* raw lv2 syscalls                                                     */
/* ------------------------------------------------------------------ */

static long t_sc4(long num, long a, long b, long c, long d)
{
    register long r11 __asm__("r11") = num;
    register long r3  __asm__("r3")  = a;
    register long r4  __asm__("r4")  = b;
    register long r5  __asm__("r5")  = c;
    register long r6  __asm__("r6")  = d;
    __asm__ __volatile__("sc"
        : "+r"(r3), "+r"(r4), "+r"(r5), "+r"(r6), "+r"(r11)
        :
        : "r0", "r7", "r8", "r9", "r10", "r12",
          "ctr", "lr", "cr0", "cr1", "cr5", "cr6", "cr7", "memory");
    return r3;
}

static void t_write(const char* buf, long len)
{
    t_sc4(403, 0, (long)buf, len, 0);        /* sys_tty_write(ch=0,...) */
}

static void t_exit(long status)
{
    t_sc4(3, status, 0, 0, 0);               /* sys_process_exit */
    for (;;) {}
}

/* ------------------------------------------------------------------ */
/* formatting (no libc)                                                 */
/* ------------------------------------------------------------------ */

static int t_strlen(const char* s)
{
    int n = 0;
    while (s[n]) n++;
    return n;
}

void t_puts(const char* s)
{
    t_write(s, t_strlen(s));
}

static char* t_puthex(char* p, unsigned long long v)
{
    static const char hexd[] = "0123456789abcdef";
    int i, started = 0;
    for (i = 60; i >= 0; i -= 4) {
        int nib = (int)((v >> i) & 0xF);
        if (nib || started || i == 0) { *p++ = hexd[nib]; started = 1; }
    }
    return p;
}

static char* t_putdec(char* p, unsigned long long v)
{
    char tmp[24];
    int n = 0;
    if (!v) { *p++ = '0'; return p; }
    while (v) { tmp[n++] = (char)('0' + (v % 10)); v /= 10; }
    while (n) *p++ = tmp[--n];
    return p;
}

static char* t_putstr(char* p, const char* s)
{
    while (*s) *p++ = *s++;
    return p;
}

/* ------------------------------------------------------------------ */
/* counters + checks                                                    */
/* ------------------------------------------------------------------ */

static unsigned long long g_pass, g_fail;

void t_section(const char* name)
{
    char buf[128], *p = buf;
    p = t_putstr(p, "== ");
    p = t_putstr(p, name);
    *p++ = '\n';
    t_write(buf, p - buf);
}

static void t_fail_line(const char* name, int idx,
                        unsigned long long got, unsigned long long want,
                        int have2, unsigned long long got2,
                        unsigned long long want2)
{
    char buf[256], *p = buf;
    g_fail++;
    p = t_putstr(p, "FAIL ");
    p = t_putstr(p, name);
    *p++ = '#';
    p = t_putdec(p, (unsigned long long)idx);
    p = t_putstr(p, " got=");
    p = t_puthex(p, got);
    p = t_putstr(p, " want=");
    p = t_puthex(p, want);
    if (have2) {
        p = t_putstr(p, " got2=");
        p = t_puthex(p, got2);
        p = t_putstr(p, " want2=");
        p = t_puthex(p, want2);
    }
    *p++ = '\n';
    t_write(buf, p - buf);
}

void t_kat(const char* name, int idx, unsigned long long got,
           unsigned long long want)
{
    if (got == want) { g_pass++; return; }
    t_fail_line(name, idx, got, want, 0, 0, 0);
}

void t_kat_ca(const char* name, int idx, unsigned long long got,
              unsigned long long want, unsigned long long got2,
              unsigned long long want2)
{
    if (got == want && got2 == want2) { g_pass++; return; }
    t_fail_line(name, idx, got, want, 1, got2, want2);
}

void t_nocrash(const char* name, int idx)
{
    (void)name; (void)idx;
    g_pass++;                 /* reaching here at all is the pass condition */
}

void t_check_str(const char* name, int idx, const char* got, const char* want)
{
    const char *a = got, *b = want;
    while (*a && *a == *b) { a++; b++; }
    if (*a == *b) { g_pass++; return; }
    /* string mismatch: print both raw */
    {
        char buf[256], *p = buf;
        g_fail++;
        p = t_putstr(p, "FAIL ");
        p = t_putstr(p, name);
        *p++ = '#';
        p = t_putdec(p, (unsigned long long)idx);
        p = t_putstr(p, " got=\"");
        p = t_putstr(p, got);
        p = t_putstr(p, "\" want=\"");
        p = t_putstr(p, want);
        p = t_putstr(p, "\"\n");
        t_write(buf, p - buf);
    }
}

/* ------------------------------------------------------------------ */
/* CRT sanity: statics initialized, locals live across calls            */
/* ------------------------------------------------------------------ */

static int g_static_init = 0x1BADB002;
static int g_bss_probe;                  /* must be 0 */

static int NOINLINE stack_probe(int depth)
{
    volatile int local = depth * 3;
    if (depth > 0)
        local += stack_probe(depth - 1);
    return local;
}

static void crt_run(void)
{
    t_section("crt");
    t_kat("static_init", 0, (unsigned long long)(unsigned)g_static_init,
          0x1BADB002ULL);
    t_kat("bss_zero", 0, (unsigned long long)(unsigned)g_bss_probe, 0);
    /* sum of 3*i for i=0..10 = 165 */
    t_kat("stack_recursion", 0, (unsigned long long)stack_probe(10), 165);
}

/* ------------------------------------------------------------------ */
/* main                                                                 */
/* ------------------------------------------------------------------ */

int main(void)
{
    t_puts("TORTURE START\n");

    crt_run();
    kat_run_all();
    torture_mem_run();
    torture_c_run();
    torture_dtoa_run();       /* last: known hang-risk area (dtoa) */

    {
        char buf[128], *p = buf;
        p = t_putstr(p, "TORTURE COMPLETE pass=");
        p = t_putdec(p, g_pass);
        p = t_putstr(p, " fail=");
        p = t_putdec(p, g_fail);
        *p++ = '\n';
        t_write(buf, p - buf);
    }

    t_exit(g_fail > 255 ? 255 : (long)g_fail);
    return 0;                 /* unreached */
}
