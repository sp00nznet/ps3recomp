#ifndef PPU_FS_AIO_ENGINE_H
#define PPU_FS_AIO_ENGINE_H

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace ppu_fs_aio {

using Id = std::int32_t;
/* Firmware-shaped IDs: mount-slot * 32 + request-slot, range 0..319.
 * Zero is valid; IDs are unique while outstanding, then may be reused. */
enum class Error {
    ok, invalid_mount, invalid_request, already_initialized, not_initialized,
    capacity, unknown_request, too_late, busy, stopping, startup_failed,
    publication_failed, engine_failure
};
enum class ResultCode { success, cancelled, io_error, engine_failure };
struct Result {
    ResultCode code = ResultCode::success;
    std::uint64_t bytes = 0;
    std::int32_t detail = 0; // Caller-owned opaque error; preserved verbatim.
};
struct Submission { Error error; Id id; };
using Work = std::function<Result()>;
using Completion = std::function<void(Id, const Result&)>;
/* Called while acceptance is locked, before work can become visible. Must
 * not call back into this engine. False/exception rejects the submission.
 * All potentially allocating queue operations precede this publication. */
using PublishId = std::function<bool(Id)>;
struct Hooks {
    /* Both run once on each mount's dedicated worker, outside the engine lock.
     * Hooks for distinct mounts can run concurrently; captured state must be
     * thread-safe. Cleanup also
     * runs after a false/throwing startup, allowing partial setup recovery.
     * Hooks must not reenter the engine or destroy its owner. */
    std::function<bool()> startup;
    std::function<void()> cleanup;
};

class Engine {
public:
    explicit Engine(Hooks hooks = {});
    ~Engine();
    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;
    static constexpr unsigned max_mounts = 10;
    static constexpr unsigned max_outstanding_per_mount = 32;

    /* Idempotent for an already initialized mount. Caller supplies canonical
     * absolute mount identities. Empty, relative,
     * trailing slash, backslash, empty/dot/dotdot components are rejected. */
    Error init(const std::string& mount);
    Submission submit(const std::string& mount, Work work,
                      Completion completion, PublishId publish);
    /* Detaches a queued request and invokes its cancelled completion on the
     * Cancel caller outside the lock, then reclaims its slot. Work never runs.
     * Executing/completing requests are too_late; reclaimed IDs are unknown. */
    Error cancel(Id id);
    /* Stops acceptance, cancels queued work with completions on the Finish
     * caller, then waits for executing work/callbacks before removal.
     * Reentrant Submit is supported, but closing mounts reject it. Calls from
     * any engine worker or callback return busy without changing state,
     * including cross-mount calls, to avoid cyclic joins. Concurrent Finish
     * is busy. Only this mount's worker is joined; other mounts stay active.
     * Once all mounts finish, Init can start fresh mount workers again. */
    Error finish(const std::string& mount);
    /* Terminal: drains queued work on every mount (unlike Finish, which cancels
     * queued work), completes callbacks, cleans up and joins every worker.
     * Idempotent; worker calls return busy without changing state. No callback
     * executes after a successful return. Destruction must be external to the
     * worker/callback; the destructor invokes this same synchronous shutdown. */
    Error shutdown();
    /* Sticky engine_failure for a throwing completion/cleanup. Work exceptions
     * instead produce one ResultCode::engine_failure completion. */
    Error health() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace ppu_fs_aio
#endif
