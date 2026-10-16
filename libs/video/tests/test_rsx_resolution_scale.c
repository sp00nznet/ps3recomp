#include "../rsx_resolution_scale.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_checks = 0;
static int g_failures = 0;

#define CHECK(cond)                                                          \
    do {                                                                     \
        ++g_checks;                                                          \
        if (!(cond)) {                                                       \
            ++g_failures;                                                    \
            fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, \
                    #cond);                                                  \
        }                                                                    \
    } while (0)

#define MARKER "/* RSX internal resolution */"
#define WINDOW "/* RSX shader window */"
#define CANARY ((char)0xA5)

static const char* const k_base =
    "struct PSInput { float4 position : SV_POSITION; float4 tc0 : TEXCOORD0; };\n"
    "float4 helper(float4 v) { return v.zwxy; }\n"
    "float4 main(PSInput input) : SV_TARGET {\n"
    "    /* RSX shader window */\n"
    "    input.position.xy = input.position.xy * float2(1.0,-1.0) + float2(0.0,720.0);\n"
    "    float d = input.position.z * input.position.w + input.position.zw.x;\n"
    "    return helper(input.tc0) + input.position + d;\n"
    "}\n"
    "// trailing suffix\n";

static size_t count_occurrences(const char* hay, const char* needle)
{
    size_t count = 0;
    size_t nlen = strlen(needle);
    const char* p = hay;
    while ((p = strstr(p, needle)) != NULL) {
        ++count;
        p += nlen;
    }
    return count;
}

static size_t brace_insert_pos(const char* src)
{
    const char* sig = strstr(src, "main(PSInput input)");
    if (sig == NULL) {
        return 0;
    }
    const char* brace = strchr(sig, '{');
    if (brace == NULL) {
        return 0;
    }
    return (size_t)(brace - src) + 1;
}

/* Independently extracts the two float2 factors following the marker. */
static int parse_factors(const char* out, double* fx, double* fy)
{
    const char* m = strstr(out, MARKER);
    if (m == NULL) {
        return -1;
    }
    const char* p = strstr(m, "float2(");
    if (p == NULL) {
        return -1;
    }
    const char* line_end = strchr(m + strlen(MARKER) + 1, '\n');
    if (line_end != NULL && p > line_end) {
        return -1;
    }
    p += 7;
    char* end = NULL;
    *fx = strtod(p, &end);
    if (end == p) {
        return -1;
    }
    while (*end == ' ') {
        ++end;
    }
    if (*end != ',') {
        return -1;
    }
    p = end + 1;
    *fy = strtod(p, &end);
    if (end == p) {
        return -1;
    }
    while (*end == ' ') {
        ++end;
    }
    if (*end != ')') {
        return -1;
    }
    return 0;
}

static int literal_is_float(const char* out)
{
    const char* p = strstr(strstr(out, MARKER), "float2(") + 7;
    for (int arg = 0; arg < 2; ++arg) {
        int has_point = 0;
        while (*p == ' ') {
            ++p;
        }
        while (*p != ',' && *p != ')' && *p != '\0') {
            if (*p == '.' || *p == 'e' || *p == 'E') {
                has_point = 1;
            } else if (!((*p >= '0' && *p <= '9') || *p == '+' || *p == '-')) {
                return 0;
            }
            ++p;
        }
        if (!has_point || *p == '\0') {
            return 0;
        }
        ++p;
    }
    return 1;
}

static void check_guest_mapping(double factor, uint32_t guest, uint32_t host)
{
    double centers[4];
    centers[0] = 0.5;
    centers[1] = (double)(host / 2u) + 0.5;
    centers[2] = (double)host - 0.5;
    centers[3] = (double)(host / 3u) + 0.5;
    for (int i = 0; i < 4; ++i) {
        double expected = centers[i] * (double)guest / (double)host;
        float physical = (float)centers[i];
        float mapped = physical * (float)factor;
        double tol = 1e-6 * (expected > 1.0 ? expected : 1.0);
        CHECK(fabs((double)mapped - expected) <= tol);
    }
}

static void check_scaled(const char* src, uint32_t gw, uint32_t gh, uint32_t hw,
                         uint32_t hh)
{
    char buf[2048];
    const uint32_t cap = 1536;
    memset(buf, CANARY, sizeof(buf));
    size_t olen = strlen(src);
    memcpy(buf, src, olen + 1);

    int r = rsx_fp_apply_resolution_scale(buf, cap, gw, gh, hw, hh);
    CHECK(r == 1);
    if (r != 1) {
        return;
    }

    int canaries_ok = 1;
    for (size_t i = cap; i < sizeof(buf); ++i) {
        if (buf[i] != CANARY) {
            canaries_ok = 0;
        }
    }
    CHECK(canaries_ok);

    size_t nlen = strlen(buf);
    CHECK(nlen > olen);
    size_t ins = nlen - olen;
    size_t pos = brace_insert_pos(src);
    CHECK(pos != 0);

    /* Prefix, body and suffix (including terminator) preserved. */
    CHECK(memcmp(buf, src, pos) == 0);
    CHECK(memcmp(buf + pos + ins, src + pos, olen - pos + 1) == 0);

    /* Inserted segment: one marker, one statement, no zw rewrite. */
    char seg[512];
    CHECK(ins < sizeof(seg));
    if (ins >= sizeof(seg)) {
        return;
    }
    memcpy(seg, buf + pos, ins);
    seg[ins] = '\0';
    CHECK(count_occurrences(seg, MARKER) == 1);
    CHECK(count_occurrences(seg, ";") == 1);
    CHECK(count_occurrences(seg, "input.position.xy") >= 1);
    CHECK(strstr(seg, "zw") == NULL);
    CHECK(strstr(seg, ".z") == NULL);
    CHECK(strstr(seg, ".w") == NULL);
    CHECK(strstr(seg, "tc0") == NULL);
    CHECK(strstr(seg, "return") == NULL);

    CHECK(count_occurrences(buf, MARKER) == 1);
    CHECK(count_occurrences(buf, "position.zw") == count_occurrences(src, "position.zw"));
    CHECK(count_occurrences(buf, "main(PSInput input)") == 1);

    const char* m = strstr(buf, MARKER);
    const char* w = strstr(buf, WINDOW);
    const char* brace = buf + pos - 1;
    CHECK(m != NULL && m > brace);
    if (w != NULL) {
        CHECK(m != NULL && m < w);
    }

    double fx = 0.0;
    double fy = 0.0;
    CHECK(parse_factors(buf, &fx, &fy) == 0);
    CHECK(literal_is_float(buf));
    double ex = (double)gw / (double)hw;
    double ey = (double)gh / (double)hh;
    CHECK(fabs(fx - ex) <= 1e-15 * ex);
    CHECK(fabs(fy - ey) <= 1e-15 * ey);
    check_guest_mapping(fx, gw, hw);
    check_guest_mapping(fy, gh, hh);

    /* A second pass must reject the duplicate marker and leave it intact. */
    char again[2048];
    memcpy(again, buf, sizeof(buf));
    CHECK(rsx_fp_apply_resolution_scale(buf, cap, gw, gh, hw, hh) == -1);
    CHECK(memcmp(again, buf, sizeof(buf)) == 0);
}

static void check_rejected(const char* src, uint32_t cap, uint32_t gw, uint32_t gh,
                           uint32_t hw, uint32_t hh)
{
    char buf[1024];
    char ref[1024];
    memset(buf, CANARY, sizeof(buf));
    memcpy(buf, src, strlen(src) + 1);
    memcpy(ref, buf, sizeof(buf));
    CHECK(rsx_fp_apply_resolution_scale(buf, cap, gw, gh, hw, hh) == -1);
    CHECK(memcmp(buf, ref, sizeof(buf)) == 0);
}

static void test_scales(void)
{
    check_scaled(k_base, 1280, 720, 3840, 2160);   /* 300% */
    check_scaled(k_base, 1280, 720, 2560, 1440);   /* 200% */
    check_scaled(k_base, 1280, 720, 1920, 1080);   /* 150% */
    check_scaled(k_base, 1920, 1080, 3840, 2160);
    check_scaled(k_base, 1280, 720, 1920, 1440);   /* nonuniform */
    check_scaled(k_base, 1280, 720, 1280, 1080);   /* one axis only */
    check_scaled(k_base, 1280, 720, 1702, 958);    /* rounded 133% */
    check_scaled(k_base, 720, 480, 1173, 782);     /* rounded odd extents */
    check_scaled(k_base, 1920, 1080, 1280, 720);   /* downscale */
    check_scaled(k_base, 4294967295u, 4294967295u, 1, 1);
    check_scaled(k_base, 1, 1, 4294967295u, 4294967295u);
    check_scaled(k_base, 4294967295u, 4294967294u, 4294967294u, 4294967295u);
}

static void test_identity(void)
{
    char buf[1024];
    char ref[1024];
    memset(buf, CANARY, sizeof(buf));
    memcpy(buf, k_base, strlen(k_base) + 1);
    memcpy(ref, buf, sizeof(buf));
    CHECK(rsx_fp_apply_resolution_scale(buf, 900, 1280, 720, 1280, 720) == 0);
    CHECK(memcmp(buf, ref, sizeof(buf)) == 0);
    CHECK(rsx_fp_apply_resolution_scale(buf, 900, 4294967295u, 4294967295u,
                                        4294967295u, 4294967295u) == 0);
    CHECK(memcmp(buf, ref, sizeof(buf)) == 0);

    /* Identity still validates: exact-fit capacity is accepted. */
    uint32_t exact = (uint32_t)strlen(k_base) + 1;
    CHECK(rsx_fp_apply_resolution_scale(buf, exact, 1280, 720, 1280, 720) == 0);
    CHECK(memcmp(buf, ref, sizeof(buf)) == 0);

    /* Validation precedes identity return. */
    check_rejected("float4 main(PSInput input) : SV_TARGET {\n    " MARKER "\n    return 0;\n}\n",
                   900, 1280, 720, 1280, 720);
    check_rejected("float4 main(PSInput inp) : SV_TARGET { return 0; }\n", 900, 1280, 720,
                   1280, 720);
    check_rejected(k_base, (uint32_t)strlen(k_base), 1280, 720, 1280, 720);
}

static void test_invalid(void)
{
    CHECK(rsx_fp_apply_resolution_scale(NULL, 100, 1280, 720, 3840, 2160) == -1);

    char buf[1024];
    char ref[1024];
    memset(buf, CANARY, sizeof(buf));
    memcpy(buf, k_base, strlen(k_base) + 1);
    memcpy(ref, buf, sizeof(buf));
    CHECK(rsx_fp_apply_resolution_scale(buf, 0, 1280, 720, 3840, 2160) == -1);
    CHECK(memcmp(buf, ref, sizeof(buf)) == 0);

    check_rejected(k_base, 900, 0, 720, 3840, 2160);
    check_rejected(k_base, 900, 1280, 0, 3840, 2160);
    check_rejected(k_base, 900, 1280, 720, 0, 2160);
    check_rejected(k_base, 900, 1280, 720, 3840, 0);
    check_rejected(k_base, 900, 0, 0, 0, 0);

    /* Missing or malformed signature. */
    check_rejected("float4 main(PSInput in) : SV_TARGET { return in.position; }\n", 900,
                   1280, 720, 3840, 2160);
    check_rejected("float4 main(VSInput input) : SV_TARGET { return 0; }\n", 900, 1280,
                   720, 3840, 2160);
    check_rejected("float4 main ( PSInput input ) : SV_TARGET { return 0; }\n", 900, 1280,
                   720, 3840, 2160);
    check_rejected("", 900, 1280, 720, 3840, 2160);

    /* Missing opening brace. */
    check_rejected("float4 main(PSInput input) : SV_TARGET\n", 900, 1280, 720, 3840, 2160);
    check_rejected("float4 main(PSInput input) : SV_TARGET;\nvoid f() { }\n", 900, 1280,
                   720, 3840, 2160);
    check_rejected("float4 main(PSInput input) : SV_TARGET }\n", 900, 1280, 720, 3840,
                   2160);

    /* Duplicate marker anywhere. */
    check_rejected("// " MARKER "\nfloat4 main(PSInput input) : SV_TARGET { return 0; }\n",
                   900, 1280, 720, 3840, 2160);

    /* Ambiguous duplicate signature. */
    check_rejected("float4 main(PSInput input) : SV_TARGET { return 0; }\n"
                   "float4 main(PSInput input) : SV_TARGET { return 1; }\n",
                   900, 1280, 720, 3840, 2160);
}

static void test_unterminated(void)
{
    char buf[1024];
    char ref[1024];
    size_t len = strlen(k_base);
    memset(buf, CANARY, sizeof(buf));
    memcpy(buf, k_base, len + 1);
    memcpy(ref, buf, sizeof(buf));
    /* NUL lies just outside capacity. */
    CHECK(rsx_fp_apply_resolution_scale(buf, (uint32_t)len, 1280, 720, 3840, 2160) == -1);
    CHECK(memcmp(buf, ref, sizeof(buf)) == 0);

    /* No NUL anywhere in the bounded region. */
    memset(buf, 'A', 64);
    memcpy(buf, "float4 main(PSInput input) { ", 29);
    buf[64] = '\0';
    memcpy(ref, buf, sizeof(buf));
    CHECK(rsx_fp_apply_resolution_scale(buf, 64, 1280, 720, 3840, 2160) == -1);
    CHECK(memcmp(buf, ref, sizeof(buf)) == 0);
}

static void test_exact_fit(void)
{
    char big[2048];
    memcpy(big, k_base, strlen(k_base) + 1);
    CHECK(rsx_fp_apply_resolution_scale(big, sizeof(big), 1280, 720, 3840, 2160) == 1);
    size_t needed = strlen(big) + 1;

    char buf[2048];
    char ref[2048];

    /* Exact fit succeeds and does not touch bytes past capacity. */
    memset(buf, CANARY, sizeof(buf));
    memcpy(buf, k_base, strlen(k_base) + 1);
    CHECK(rsx_fp_apply_resolution_scale(buf, (uint32_t)needed, 1280, 720, 3840, 2160) == 1);
    CHECK(strcmp(buf, big) == 0);
    int canaries_ok = 1;
    for (size_t i = needed; i < sizeof(buf); ++i) {
        if (buf[i] != CANARY) {
            canaries_ok = 0;
        }
    }
    CHECK(canaries_ok);

    /* One byte short fails and leaves every byte unchanged. */
    memset(buf, CANARY, sizeof(buf));
    memcpy(buf, k_base, strlen(k_base) + 1);
    memcpy(ref, buf, sizeof(buf));
    CHECK(rsx_fp_apply_resolution_scale(buf, (uint32_t)(needed - 1), 1280, 720, 3840, 2160) ==
          -1);
    CHECK(memcmp(buf, ref, sizeof(buf)) == 0);
}

static void test_minimal_and_no_position_read(void)
{
    /* Spec example shape. */
    check_scaled("struct PSInput { float4 position : SV_POSITION; };\n"
                 "float4 main(PSInput input) : SV_TARGET {\n"
                 "    /* RSX shader window */\n"
                 "    input.position.xy = input.position.xy * float2(1.0,-1.0) + float2(0.0,720.0);\n"
                 "    return input.position;\n"
                 "}\n",
                 1280, 720, 3840, 2160);
    /* Shader that never reads position; brace on its own line. */
    check_scaled("struct PSInput { float4 position : SV_POSITION; float4 c : COLOR0; };\n"
                 "float4 main(PSInput input) : SV_TARGET\n{\n    return input.c;\n}",
                 1280, 720, 1920, 1080);
}

int main(void)
{
    test_scales();
    test_identity();
    test_invalid();
    test_unterminated();
    test_exact_fit();
    test_minimal_and_no_position_read();

    printf("rsx_resolution_scale: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
