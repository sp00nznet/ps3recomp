#include "port_extension.h"
#include <cstddef>
#include <mutex>

namespace {
/* With one registrant a slot holds its function directly; with several it
 * holds a fan-out that calls each in registration order (stop in reverse);
 * with none it stays null. Registration normally happens in static
 * constructors, before main, so the lock only guards unusual late callers. */
constexpr std::size_t kMaxRegistrants = 8;

template <typename Fn> struct Fanout {
    Fn fn[kMaxRegistrants]{};
    std::size_t count{};
};

PortExtensionCallbacks registry{};
Fanout<void (*)(const char*)> prepares, starts;
Fanout<void (*)(void)> polls, stops;
std::mutex registry_lock;

void fan_prepare(const char* elf) { for (std::size_t i = 0; i < prepares.count; ++i) prepares.fn[i](elf); }
void fan_start(const char* elf) { for (std::size_t i = 0; i < starts.count; ++i) starts.fn[i](elf); }
void fan_poll(void) { for (std::size_t i = 0; i < polls.count; ++i) polls.fn[i](); }
void fan_stop(void) { for (std::size_t i = stops.count; i > 0; --i) stops.fn[i - 1](); }

template <typename Fn> bool full(const Fanout<Fn>& list, Fn incoming)
{
    return incoming && list.count == kMaxRegistrants;
}

template <typename Fn> void add(Fanout<Fn>& list, Fn& slot, Fn incoming, Fn fan)
{
    if (!incoming) return;
    list.fn[list.count++] = incoming;
    slot = list.count == 1 ? incoming : fan;
}
}  // namespace

extern "C" int port_extension_register(const PortExtensionCallbacks* c)
{
    if (!c) return -1;
    std::lock_guard<std::mutex> guard(registry_lock);
    if (full(prepares, c->prepare) || full(starts, c->start) ||
        full(polls, c->frame_poll) || full(stops, c->stop))
        return -1;
    add(prepares, registry.prepare, c->prepare, fan_prepare);
    add(starts, registry.start, c->start, fan_start);
    add(polls, registry.frame_poll, c->frame_poll, fan_poll);
    add(stops, registry.stop, c->stop, fan_stop);
    return 0;
}

extern "C" const PortExtensionCallbacks* port_extension(void) { return &registry; }
