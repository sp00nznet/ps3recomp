/* Synthetic checks for the build-extension registry (port_extension.cpp). */
#include "../port_extension.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

static std::string g_log;
static int g_failures;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); g_failures++; } } while (0)

static void prep_a(const char* elf) { g_log += std::string("pa:") + elf + ";"; }
static void prep_b(const char* elf) { g_log += std::string("pb:") + elf + ";"; }
static void start_a(const char* elf) { g_log += std::string("sa:") + elf + ";"; }
static void poll_a(void) { g_log += "fa;"; }
static void poll_b(void) { g_log += "fb;"; }
static void stop_a(void) { g_log += "xa;"; }
static void stop_b(void) { g_log += "xb;"; }
static void stop_c(void) { g_log += "xc;"; }

int main()
{
    const PortExtensionCallbacks* r = port_extension();
    CHECK(r != nullptr);
    CHECK(!r->prepare && !r->start && !r->frame_poll && !r->stop);   /* inert by default */
    CHECK(port_extension_register(nullptr) == -1);

    PortExtensionCallbacks a{}; a.prepare = prep_a; a.start = start_a; a.frame_poll = poll_a; a.stop = stop_a;
    CHECK(port_extension_register(&a) == 0);
    /* One registrant: the slot is that function, with no fan-out in between. */
    CHECK(r->prepare == prep_a && r->start == start_a && r->frame_poll == poll_a && r->stop == stop_a);

    PortExtensionCallbacks b{}; b.prepare = prep_b; b.frame_poll = poll_b; b.stop = stop_b;
    CHECK(port_extension_register(&b) == 0);
    CHECK(r->start == start_a);                 /* untouched slot keeps its single owner */
    CHECK(r->prepare != prep_a && r->stop != stop_a);
    g_log.clear();
    r->prepare("game.elf"); r->start("game.elf"); r->frame_poll(); r->stop();
    CHECK(g_log == "pa:game.elf;pb:game.elf;sa:game.elf;fa;fb;xb;xa;");

    /* Fill stop to eight, then a ninth registration that also adds a poll is
     * refused as a whole: neither slot changes. */
    PortExtensionCallbacks c{}; c.stop = stop_c;
    for (int i = 0; i < 6; i++) CHECK(port_extension_register(&c) == 0);
    PortExtensionCallbacks d{}; d.frame_poll = poll_a; d.stop = stop_c;
    CHECK(port_extension_register(&d) == -1);
    g_log.clear();
    r->frame_poll(); r->stop();
    CHECK(g_log == "fa;fb;xc;xc;xc;xc;xc;xc;xb;xa;");

    /* An empty registration is accepted and changes nothing. */
    PortExtensionCallbacks empty{};
    CHECK(port_extension_register(&empty) == 0);

    if (g_failures) { std::printf("port_extension: %d failure(s)\n", g_failures); return 1; }
    std::printf("port_extension: registry checks passed\n");
    return 0;
}
