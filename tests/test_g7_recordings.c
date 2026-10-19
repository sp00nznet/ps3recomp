#include "g7_motion_backend.h"
#include "pad_motion_g7.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
/* CPU-only replay of user-labelled restart recordings. This verifies decoding
 * and calibrated DS3 ranges, not Folklore's unidentified reel-in threshold. */
int main(int argc, char **argv) {
    assert(argc == 3);
    int flat_min = 1024, flat_max = 0, flick_min = 1024, flick_max = 0;
    for (int file = 1; file < argc; ++file) {
        FILE *f = fopen(argv[file], "r"); assert(f);
        char line[512]; assert(fgets(line, sizeof(line), f));
        int count = 0, lo = 1024, hi = 0;
        while (fgets(line, sizeof(line), f)) {
            char *hex = strchr(line, ','); assert(hex); ++hex;
            assert(strcspn(hex, "\r\n") == 128);
            unsigned char data[64];
            for (int i = 0; i < 64; ++i) {
                char byte[3] = {hex[2*i], hex[2*i+1], 0};
                data[i] = (unsigned char)strtoul(byte, NULL, 16);
            }
            g7_motion_sample raw; uint16_t pad[4];
            assert(g7_motion_test_decode(data, 64, &raw));
            pad_motion_g7_map(&raw, pad);
            for (int axis = 0; axis < 4; ++axis) assert(pad[axis] <= 1023);
            assert(pad[3] == 512);
            if (pad[1] < lo) lo = pad[1];
            if (pad[1] > hi) hi = pad[1];
            ++count;
        }
        fclose(f); assert(count > 1000);
        printf("%s: %d reports, SIXAXIS Y=%d..%d\n", file == 1 ? "flat" : "flick", count, lo, hi);
        if (file == 1) { flat_min = lo; flat_max = hi; }
        else { flick_min = lo; flick_max = hi; }
    }
    assert(flat_min >= 390 && flat_max <= 408);
    assert(flick_min < flat_min - 150 && flick_max > flat_max + 150);
    puts("PASS: physical acceleration distinct from flat; game threshold still unverified");
    return 0;
}
