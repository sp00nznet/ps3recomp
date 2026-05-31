/*
 * ps3recomp - NV40 vertex-program decode layout validation
 *
 * Confirms the VP instruction bitfield layout (from RPCS3 RSXVertexProgram.h)
 * against a corpus of real vertex programs (RPCS3 shader cache raw/ *.vp).
 *
 * VP words are stored little-endian (RPCS3 native order), 4 words = 16 bytes
 * per instruction, NO inline constants (VP constants live in a separate bank),
 * program ends at the D3.end bit. If the layout is right, every program
 * terminates within its file and every vec/sca opcode is known.
 *
 * Build: gcc -std=c11 -O2 validate_vp_decode.c -o vvp.exe
 * Run:   ./vvp.exe "<.../shaders_cache/raw>"
 */
#include <dirent.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef uint32_t u32; typedef uint8_t u8;

static u32 rd_le(const u8* p)
{ return (u32)p[0] | ((u32)p[1]<<8) | ((u32)p[2]<<16) | ((u32)p[3]<<24); }

static const char* vec_name(u32 op) {
    static const char* n[] = {"NOP","MOV","MUL","ADD","MAD","DP3","DPH","DP4",
        "DST","MIN","MAX","SLT","SGE","ARL","FRC","FLR","SEQ","SFL","SGT","SLE",
        "SNE","STR","SSG","?23","?24","TXL"};
    return op < 26 ? n[op] : "??";
}
static const char* sca_name(u32 op) {
    static const char* n[] = {"NOP","MOV","RCP","RCC","RSQ","EXP","LOG","LIT",
        "BRA","BRI","CAL","CLI","RET","LG2","EX2","SIN","COS","BRB","CLB","PSH","POP"};
    return op < 21 ? n[op] : "??";
}

static int vec_hist[32], sca_hist[32];
static u8 buf[65536];

int main(int argc, char** argv)
{
    if (argc < 2) { printf("usage: %s <dir>\n", argv[0]); return 2; }
    DIR* d = opendir(argv[1]);
    if (!d) { printf("cannot open %s\n", argv[1]); return 1; }

    int total=0, terminated=0, unknown_op=0, no_end=0, max_instr=0;
    struct dirent* e; char path[1024];
    while ((e = readdir(d))) {
        size_t L = strlen(e->d_name);
        if (L < 3 || strcmp(e->d_name+L-3, ".vp")) continue;
        snprintf(path, sizeof(path), "%s/%s", argv[1], e->d_name);
        FILE* f = fopen(path, "rb"); if (!f) continue;
        long n = (long)fread(buf, 1, sizeof(buf), f); fclose(f);
        if (n < 16) continue;
        total++;

        int ended = 0, instrs = 0, file_unknown = 0;
        for (long off = 0; off + 16 <= n; off += 16) {
            u32 w1 = rd_le(buf+off+4), w3 = rd_le(buf+off+12);
            u32 vop = (w1 >> 22) & 0x1F;
            u32 sop = (w1 >> 27) & 0x1F;
            vec_hist[vop]++; sca_hist[sop]++;
            if (vec_name(vop)[0]=='?' || sca_name(sop)[0]=='?') file_unknown++;
            instrs++;
            if (w3 & 1u) { ended = 1; break; }
        }
        if (instrs > max_instr) max_instr = instrs;
        if (ended) terminated++; else no_end++;
        if (file_unknown) unknown_op++;
    }
    closedir(d);

    printf("=== VP decode validation: %s ===\n", argv[1]);
    printf("total .vp        : %d\n", total);
    printf("terminated (end) : %d  (%.1f%%)\n", terminated, total?100.0*terminated/total:0);
    printf("no end bit       : %d\n", no_end);
    printf("files w/ unknown : %d\n", unknown_op);
    printf("max instr/prog   : %d\n", max_instr);

    printf("\n=== VEC opcode histogram ===\n");
    for (int i = 0; i < 32; i++) if (vec_hist[i])
        printf("  %-5s (0x%02X): %5d%s\n", vec_name(i), i, vec_hist[i],
               vec_name(i)[0]=='?'?"  <-- UNKNOWN":"");
    printf("\n=== SCA opcode histogram ===\n");
    for (int i = 0; i < 32; i++) if (sca_hist[i])
        printf("  %-5s (0x%02X): %5d%s\n", sca_name(i), i, sca_hist[i],
               sca_name(i)[0]=='?'?"  <-- UNKNOWN":"");
    return 0;
}
