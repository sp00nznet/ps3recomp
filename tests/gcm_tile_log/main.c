/*
 * ps3recomp - tests/gcm_tile_log
 *
 * cellGcmSetTile, SetZcull, BindTile, BindZcull, UnbindTile and UnbindZcull
 * print a line per call only when ps3_log_verbose() is on (PS3_VERBOSE=1).
 * stdout is redirected to a file around the six calls and read back.
 *
 *   test_gcm_tile_log quiet     PS3_VERBOSE=0: no output, every call returns CELL_OK
 *   test_gcm_tile_log verbose   PS3_VERBOSE=1: one line per call
 *
 * Exit code 0 when every check passes.
 */
#include "cellGcmSys.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <io.h>
#define dup _dup
#define dup2 _dup2
#define close _close
#define fileno _fileno
#else
#include <unistd.h>
#endif

uint8_t* vm_base = NULL;

static int g_fail;
#define CHECK(c, ...) do { if (c) printf("[PASS] " __VA_ARGS__); \
                           else { g_fail++; printf("[FAIL] " __VA_ARGS__); } printf("\n"); } while (0)

int main(int argc, char** argv)
{
    const int want_lines = argc > 1 && !strcmp(argv[1], "verbose");
    if (argc < 2 || (!want_lines && strcmp(argv[1], "quiet"))) {
        fprintf(stderr, "usage: %s quiet|verbose\n", argv[0]);
        return 2;
    }
    setenv("PS3_VERBOSE", want_lines ? "1" : "0", 1);
    setvbuf(stdout, NULL, _IONBF, 0);

    FILE* cap = tmpfile();
    if (!cap) { printf("[FAIL] tmpfile\n"); return 1; }
    fflush(stdout);
    const int saved = dup(fileno(stdout));
    dup2(fileno(cap), fileno(stdout));

    s32 rc[6];
    rc[0] = cellGcmSetTile(1, 0, 0x100000, 0x100000, 0x400, 0, 0, 0);
    rc[1] = cellGcmSetZcull(1, 0x200000, 1280, 720, 0, 0, 0, 0, 0, 0, 0, 0);
    rc[2] = cellGcmBindTile(1);
    rc[3] = cellGcmBindZcull(1);
    rc[4] = cellGcmUnbindTile(1);
    rc[5] = cellGcmUnbindZcull(1);

    fflush(stdout);
    dup2(saved, fileno(stdout));
    close(saved);

    int lines = 0, c;
    rewind(cap);
    while ((c = fgetc(cap)) != EOF) if (c == '\n') lines++;
    fclose(cap);

    int ok = 1;
    for (int i = 0; i < 6; i++) ok &= (rc[i] == CELL_OK);
    CHECK(ok, "all six calls return CELL_OK");
    if (want_lines) CHECK(lines == 6, "PS3_VERBOSE=1: %d lines for six calls (want 6)", lines);
    else            CHECK(lines == 0, "PS3_VERBOSE=0: %d lines for six calls (want 0)", lines);

    printf("[gcm_tile_log %s] %s (%d failed)\n", argv[1], g_fail ? "FAIL" : "PASS", g_fail);
    return g_fail ? 1 : 0;
}
