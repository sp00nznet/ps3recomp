/* SPIR-V disk cache (libs/video/rsx_shader_spirv.cpp). CPU only, needs glslang.
 * `test_spirv_cache <empty scratch dir>`: a hit returns the same words as the
 * translation, damaged files are rejected and rewritten, stages do not share
 * entries, and failed translations are never stored. */
#include "rsx_shader_spirv.h"

#include <filesystem>
#include <fstream>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <vector>

static int g_fail = 0, g_checks = 0;
#define CHECK(c) do { g_checks++; if (!(c)) { g_fail++; fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, #c); } } while (0)

static const char* kVs =
    "struct VSOut { float4 pos : SV_Position; float4 col : TEXCOORD0; };\n"
    "VSOut main(float4 p : ATTR0, float4 c : ATTR3) { VSOut o; o.pos = p; o.col = c; return o; }\n";
static const char* kPs = "float4 main(float4 pos : SV_Position, float4 col : TEXCOORD0) : SV_Target { return col * 0.5; }\n";

static std::vector<u32> translate(const char* src, int stage, int* rc)
{
    std::vector<u32> w(1u << 16);
    u32 n = 0; char log[512] = "";
    *rc = rsx_hlsl_to_spirv(src, stage, w.data(), (u32)w.size(), &n, log, sizeof log);
    w.resize(*rc == 0 ? n : 0);
    return w;
}

static std::vector<std::filesystem::path> files(const std::filesystem::path& d)
{
    std::vector<std::filesystem::path> out;
    for (auto& e : std::filesystem::directory_iterator(d)) out.push_back(e.path());
    return out;
}

int main(int argc, char** argv)
{
    if (argc != 2) { fprintf(stderr, "usage: %s <empty scratch dir>\n", argv[0]); return 2; }
    const std::filesystem::path dir = std::filesystem::path(argv[1]) / "spirv-cache";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
#if defined(_WIN32)
    _putenv_s("PS3RECOMP_SHADER_CACHE_DIR", dir.string().c_str());
#else
    setenv("PS3RECOMP_SHADER_CACHE_DIR", dir.string().c_str(), 1);
#endif
    if (!rsx_hlsl_to_spirv_available()) { fprintf(stderr, "glslang not built in\n"); return 2; }
    u32 hits, misses, stores, rejects;
    int rc;

    std::vector<u32> a = translate(kVs, RSX_SHADER_STAGE_VERTEX, &rc);
    CHECK(rc == 0 && a.size() > 5 && a[0] == 0x07230203u);
    rsx_spirv_cache_stats(&hits, &misses, &stores, &rejects);
    CHECK(hits == 0 && misses == 1 && stores == 1);
    CHECK(files(dir).size() == 1);

    std::vector<u32> b = translate(kVs, RSX_SHADER_STAGE_VERTEX, &rc);
    rsx_spirv_cache_stats(&hits, &misses, &stores, &rejects);
    CHECK(rc == 0 && b == a);
    CHECK(hits == 1 && misses == 1);

    /* Same text as a fragment program is a different entry. */
    std::vector<u32> c = translate(kPs, RSX_SHADER_STAGE_FRAGMENT, &rc);
    CHECK(rc == 0 && !c.empty() && c != a);
    CHECK(files(dir).size() == 2);

    /* Damage the vertex entry's payload: rejected, retranslated, rewritten. */
    std::filesystem::path vfile;
    for (auto& p : files(dir)) if (p.string().find("-v.spv") != std::string::npos) vfile = p;
    CHECK(!vfile.empty());
    {
        std::fstream f(vfile, std::ios::in | std::ios::out | std::ios::binary);
        f.seekp(-4, std::ios::end); const char junk[4] = { 1, 2, 3, 4 }; f.write(junk, 4);
    }
    rsx_spirv_cache_stats(&hits, &misses, &stores, &rejects);
    const u32 rejects0 = rejects, misses0 = misses;
    std::vector<u32> d = translate(kVs, RSX_SHADER_STAGE_VERTEX, &rc);
    rsx_spirv_cache_stats(&hits, &misses, &stores, &rejects);
    CHECK(rc == 0 && d == a);
    CHECK(rejects == rejects0 + 1 && misses == misses0 + 1);
    std::vector<u32> e = translate(kVs, RSX_SHADER_STAGE_VERTEX, &rc);
    rsx_spirv_cache_stats(&hits, &misses, &stores, &rejects);
    CHECK(rc == 0 && e == a && rejects == rejects0 + 1 && misses == misses0 + 1);

    /* Truncated file: rejected the same way. */
    std::filesystem::resize_file(vfile, 20);
    std::vector<u32> g = translate(kVs, RSX_SHADER_STAGE_VERTEX, &rc);
    rsx_spirv_cache_stats(&hits, &misses, &stores, &rejects);
    CHECK(rc == 0 && g == a && rejects == rejects0 + 2);

    /* A translation failure is reported and nothing is stored for it. */
    const size_t before = files(dir).size();
    std::vector<u32> bad = translate("this is not hlsl", RSX_SHADER_STAGE_FRAGMENT, &rc);
    CHECK(rc != 0 && bad.empty());
    CHECK(files(dir).size() == before);
    for (auto& p : files(dir)) CHECK(p.extension() == ".spv");   /* no temp files left */

    /* A one-character change in the source is a different entry. */
    std::string vs2 = kVs; vs2[vs2.size() - 3] = ' ';
    std::vector<u32> h = translate(vs2.c_str(), RSX_SHADER_STAGE_VERTEX, &rc);
    CHECK(rc == 0 && files(dir).size() == before + 1);

    printf("spirv cache: %d checks, %d failures\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
