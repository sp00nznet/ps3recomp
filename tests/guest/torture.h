/* ps3recomp lifter torture suite -- guest-side framework.
 *
 * Reporting goes through raw lv2 syscalls (sys_tty_write via `sc`), NOT
 * libc, so a mis-lifted libc can't corrupt the reporting of its own bugs.
 * Output protocol (parsed by tests/run_torture.py):
 *   == <section>
 *   FAIL <name>#<idx> got=<hex> want=<hex> [got2=<hex> want2=<hex>]
 *   TORTURE COMPLETE pass=<n> fail=<n>
 * Passing tests are counted, not printed (3.5k+ KATs).
 */
#ifndef TORTURE_H
#define TORTURE_H

#define NOINLINE __attribute__((noinline))

void t_section(const char* name);
void t_kat(const char* name, int idx, unsigned long long got, unsigned long long want);
void t_kat_ca(const char* name, int idx, unsigned long long got, unsigned long long want, unsigned long long got2, unsigned long long want2);
void t_nocrash(const char* name, int idx);
void t_check_str(const char* name, int idx, const char* got, const char* want);
void t_puts(const char* s);              /* raw tty write, no libc */

/* generated (torture_kats.c) */
void kat_run_all(void);
/* hand-written suites */
void torture_mem_run(void);              /* torture_mem.c */
void torture_c_run(void);                /* torture_c.c */
void torture_dtoa_run(void);             /* torture_c.c -- run LAST (hang risk) */

#endif