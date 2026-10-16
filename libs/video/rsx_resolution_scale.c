#include "rsx_resolution_scale.h"

#include <stddef.h>
#include <stdio.h>
#include <string.h>

#define RSX_RES_MARKER "/* RSX internal resolution */"
#define RSX_RES_SIGNATURE "main(PSInput input)"

/* Formats a factor as a locale-independent HLSL float literal. */
static int format_factor(char* out, size_t out_size, double value)
{
    int n = snprintf(out, out_size, "%.17g", value);
    if (n <= 0 || (size_t)n >= out_size - 2) {
        return -1;
    }

    int has_point = 0;
    int written = 0;
    for (int i = 0; i < n; ++i) {
        char c = out[i];
        int keep = (c >= '0' && c <= '9') || c == 'e' || c == 'E' ||
                   c == '+' || c == '-';
        if (!keep) {
            /* Collapse the entire locale separator, including multibyte ones. */
            if (!has_point) out[written++] = '.';
            has_point = 1;
        } else {
            out[written++] = c;
            if (c == 'e' || c == 'E') has_point = 1;
        }
    }
    n = written;
    out[n] = '\0';

    if (!has_point) {
        out[n] = '.';
        out[n + 1] = '0';
        out[n + 2] = '\0';
    }
    return 0;
}

int rsx_fp_apply_resolution_scale(char* hlsl, uint32_t capacity,
                                  uint32_t guest_width, uint32_t guest_height,
                                  uint32_t host_width, uint32_t host_height)
{
    if (hlsl == NULL || capacity == 0) {
        return -1;
    }
    if (guest_width == 0 || guest_height == 0 || host_width == 0 ||
        host_height == 0) {
        return -1;
    }

    /* Bounded NUL search; never reads past capacity. */
    const char* nul = memchr(hlsl, '\0', capacity);
    if (nul == NULL) {
        return -1;
    }
    size_t len = (size_t)(nul - hlsl);

    if (strstr(hlsl, RSX_RES_MARKER) != NULL) {
        return -1;
    }

    const char* sig = strstr(hlsl, RSX_RES_SIGNATURE);
    if (sig == NULL) {
        return -1;
    }
    const char* sig_end = sig + (sizeof(RSX_RES_SIGNATURE) - 1);
    if (strstr(sig_end, RSX_RES_SIGNATURE) != NULL) {
        return -1;
    }

    const char* brace = sig_end;
    while (*brace != '{') {
        if (*brace == '\0' || *brace == ';' || *brace == '}') {
            return -1;
        }
        ++brace;
    }
    size_t pos = (size_t)(brace - hlsl) + 1;

    if (guest_width == host_width && guest_height == host_height) {
        return 0;
    }

    char fx[48];
    char fy[48];
    if (format_factor(fx, sizeof(fx), (double)guest_width / (double)host_width) != 0 ||
        format_factor(fy, sizeof(fy), (double)guest_height / (double)host_height) != 0) {
        return -1;
    }

    char block[192];
    int n = snprintf(block, sizeof(block),
                     "\n    " RSX_RES_MARKER
                     "\n    input.position.xy = input.position.xy * float2(%s, %s);",
                     fx, fy);
    if (n <= 0 || (size_t)n >= sizeof(block)) {
        return -1;
    }
    size_t block_len = (size_t)n;

    /* len <= capacity - 1, so this subtraction cannot underflow. */
    size_t room = (size_t)capacity - 1 - len;
    if (block_len > room) {
        return -1;
    }

    memmove(hlsl + pos + block_len, hlsl + pos, len - pos + 1);
    memcpy(hlsl + pos, block, block_len);
    return 1;
}
