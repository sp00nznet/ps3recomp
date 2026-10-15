#include "ppu_fs_aio_engine.h"

#include <condition_variable>
#include <array>
#include <exception>
#include <list>
#include <map>
#include <mutex>
#include <thread>
#include <utility>

namespace ppu_fs_aio {
namespace {
thread_local const void* active_completion = nullptr;
struct CompletionScope {
    const void* previous;
    explicit CompletionScope(const void* engine) : previous(active_completion) { active_completion = engine; }
    ~CompletionScope() { active_completion = previous; }
};
bool canonical_mount(const std::string& mount) {
    if (mount.empty() || mount[0] != '/' || mount.back() == '/' ||
        mount.find('\\') != std::string::npos || mount.find('\0') != std::string::npos)
        return false;
    std::size_t start = 1;
    while (start < mount.size()) {
        const auto end = mount.find('/', start);
        const auto count = (end == std::string::npos ? mount.size() : end) - start;
        if (!count || (count == 1 && mount[start] == '.') ||
            (count == 2 && mount[start] == '.' && mount[start + 1] == '.')) return false;
        if (end == std::string::npos) break;
        start = end + 1;
    }
    return true;
}
}

struct Engine::Impl {
    struct Request;
    enum class State { starting, running, failed, stopped };
    struct Mount {
        unsigned slot = 0;
        bool closing = false;
        bool stop = false;
        unsigned outstanding = 0;
        State state = State::starting;
        std::list<std::shared_ptr<Request>> queue;
        std::thread worker;
        std::thread::id worker_id;
        std::mutex join_mutex;
    };
    enum class Phase { queued, executing, completing };
    struct Request {
        Id id = 0;
        std::shared_ptr<Mount> mount;
        Work work;
        Completion completion;
        Phase phase = Phase::queued;
    };
    explicit Impl(Hooks value) : hooks(std::move(value)) {}
    Hooks hooks;
    mutable std::mutex mutex;
    std::mutex join_mutex;
    std::condition_variable changed;
    bool stop = false;
    Error sticky_error = Error::ok;
    std::map<std::string, std::shared_ptr<Mount>> mounts;
    std::map<Id, std::shared_ptr<Request>> requests;

    bool on_worker() const {
        for (const auto& item : mounts)
            if (item.second->worker_id == std::this_thread::get_id()) return true;
        return false;
    }
    static void join(const std::shared_ptr<Mount>& mount) {
        std::lock_guard<std::mutex> lock(mount->join_mutex);
        if (mount->worker.joinable()) mount->worker.join();
    }

    void complete(const std::shared_ptr<Request>& request, const Result& result) noexcept {
        {
            CompletionScope scope(this);
            try { request->completion(request->id, result); }
            catch (...) { std::lock_guard<std::mutex> lock(mutex); sticky_error = Error::engine_failure; }
        }
        std::lock_guard<std::mutex> lock(mutex);
        --request->mount->outstanding;
        requests.erase(request->id);
        changed.notify_all();
    }

    void cleanup() noexcept {
        try { if (hooks.cleanup) hooks.cleanup(); }
        catch (...) { std::lock_guard<std::mutex> lock(mutex); sticky_error = Error::engine_failure; }
    }
    void run(const std::shared_ptr<Mount>& mount) noexcept {
        {
            std::lock_guard<std::mutex> lock(mutex);
            mount->worker_id = std::this_thread::get_id();
        }
        bool ready = false;
        try { ready = !hooks.startup || hooks.startup(); } catch (...) {}
        if (!ready) {
            cleanup();
            std::lock_guard<std::mutex> lock(mutex);
            mount->worker_id = {};
            mount->state = State::failed;
            changed.notify_all();
            return;
        }
        {
            std::lock_guard<std::mutex> lock(mutex);
            mount->state = State::running;
            changed.notify_all();
        }
        for (;;) {
            std::shared_ptr<Request> request;
            {
                std::unique_lock<std::mutex> lock(mutex);
                changed.wait(lock, [&] { return mount->stop || !mount->queue.empty(); });
                if (mount->queue.empty() && mount->stop) break;
                request = mount->queue.front();
                mount->queue.pop_front();
                request->phase = Phase::executing;
            }
            Result result;
            try { result = request->work(); }
            catch (...) { result = {ResultCode::engine_failure, 0}; }
            {
                std::lock_guard<std::mutex> lock(mutex);
                request->phase = Phase::completing;
            }
            complete(request, result);
        }
        cleanup();
        {
            std::lock_guard<std::mutex> lock(mutex);
            mount->worker_id = {};
            mount->state = State::stopped;
            changed.notify_all();
        }
    }
};

Engine::Engine(Hooks hooks) : impl_(new Impl(std::move(hooks))) {}
Engine::~Engine() {
    /* Destroying the object from its own callback cannot synchronously drain
     * and join itself. Make that ownership violation explicit, never detach a
     * worker that still refers to destroyed engine/guest state. */
    if (shutdown() == Error::busy) std::terminate();
}

Error Engine::init(const std::string& mount) {
    if (!canonical_mount(mount)) return Error::invalid_mount;
    auto& p = *impl_;
    std::unique_lock<std::mutex> lock(p.mutex);
    if (p.stop) return Error::stopping;
    if (p.on_worker() || active_completion == &p) return Error::busy;
    auto existing = p.mounts.find(mount);
    if (existing != p.mounts.end()) {
        auto state = existing->second;
        p.changed.wait(lock, [&] { return state->state != Impl::State::starting; });
        if (p.stop) return Error::stopping;
        if (state->closing) return Error::busy;
        return state->state == Impl::State::running ? Error::ok : Error::startup_failed;
    }
    if (p.mounts.size() >= max_mounts) return Error::capacity;
    unsigned slot = 0;
    for (; slot < max_mounts; ++slot) {
        bool used = false;
        for (const auto& item : p.mounts) if (item.second->slot == slot) { used = true; break; }
        if (!used) break;
    }
    std::shared_ptr<Impl::Mount> state;
    try {
        state = std::make_shared<Impl::Mount>();
        state->slot = slot;
        p.mounts.emplace(mount, state);
    }
    catch (...) { return Error::engine_failure; }
    try { state->worker = std::thread([&p, state] { p.run(state); }); }
    catch (...) { state->state = Impl::State::failed; p.changed.notify_all(); }
    p.changed.wait(lock, [&] { return state->state != Impl::State::starting; });
    if (p.stop) return Error::stopping;
    if (state->closing) return Error::busy;
    if (state->state == Impl::State::running) return Error::ok;
    lock.unlock();
    Impl::join(state);
    lock.lock();
    auto found = p.mounts.find(mount);
    if (found != p.mounts.end() && found->second == state) p.mounts.erase(found);
    return Error::startup_failed;
}

Submission Engine::submit(const std::string& mount, Work work,
                          Completion completion, PublishId publish) {
    if (!work || !completion || !publish) return {Error::invalid_request, 0};
    auto& p = *impl_;
    std::shared_ptr<Impl::Request> request;
    std::list<std::shared_ptr<Impl::Request>> pending;
    try {
        request = std::make_shared<Impl::Request>();
        request->work = std::move(work);
        request->completion = std::move(completion);
        pending.push_back(request);
    } catch (...) { return {Error::engine_failure, 0}; }
    std::unique_lock<std::mutex> lock(p.mutex);
    if (p.stop) return {Error::stopping, 0};
    auto found = p.mounts.find(mount);
    if (found == p.mounts.end()) return {Error::not_initialized, 0};
    if (found->second->closing) return {Error::busy, 0};
    if (found->second->state != Impl::State::running) return {Error::not_initialized, 0};
    if (found->second->outstanding >= max_outstanding_per_mount) return {Error::capacity, 0};
    Id id = -1;
    for (unsigned slot = 0; slot < max_outstanding_per_mount; ++slot) {
        const auto candidate = static_cast<Id>(found->second->slot * max_outstanding_per_mount + slot);
        if (!p.requests.count(candidate)) { id = candidate; break; }
    }
    if (id < 0) return {Error::capacity, 0};
    request->id = id;
    request->mount = found->second;
    try { p.requests.emplace(id, request); }
    catch (...) { return {Error::engine_failure, 0}; }
    bool published = false;
    try { published = publish(id); } catch (...) {}
    if (!published) { p.requests.erase(id); return {Error::publication_failed, 0}; }
    ++request->mount->outstanding;
    request->mount->queue.splice(request->mount->queue.end(), pending);
    p.changed.notify_all();
    return {Error::ok, id};
}

Error Engine::cancel(Id id) {
    if (id < 0 || id >= static_cast<Id>(max_mounts * max_outstanding_per_mount)) return Error::invalid_request;
    auto& p = *impl_;
    std::unique_lock<std::mutex> lock(p.mutex);
    auto found = p.requests.find(id);
    if (found == p.requests.end()) return Error::unknown_request;
    if (found->second->phase != Impl::Phase::queued) return Error::too_late;
    auto request = found->second;
    request->phase = Impl::Phase::completing;
    request->mount->queue.remove(request);
    lock.unlock();
    p.complete(request, {ResultCode::cancelled, 0});
    return Error::ok;
}

Error Engine::finish(const std::string& mount) {
    auto& p = *impl_;
    std::unique_lock<std::mutex> lock(p.mutex);
    if (p.on_worker() || active_completion == &p) return Error::busy;
    if (p.stop) return Error::stopping;
    auto found = p.mounts.find(mount);
    if (found == p.mounts.end()) return Error::not_initialized;
    auto state = found->second;
    if (state->closing) return Error::busy;
    state->closing = true;
    state->stop = true;
    std::list<std::shared_ptr<Impl::Request>> cancelled;
    for (const auto& request : state->queue) request->phase = Impl::Phase::completing;
    cancelled.splice(cancelled.end(), state->queue);
    p.changed.notify_all();
    lock.unlock();
    for (const auto& request : cancelled) p.complete(request, {ResultCode::cancelled, 0});
    Impl::join(state);
    lock.lock();
    p.changed.wait(lock, [&] { return state->outstanding == 0; });
    auto current = p.mounts.find(mount);
    if (current != p.mounts.end() && current->second == state) p.mounts.erase(current);
    return Error::ok;
}

Error Engine::shutdown() {
    auto& p = *impl_;
    {
        std::lock_guard<std::mutex> lock(p.mutex);
        if (p.on_worker() || active_completion == &p) return Error::busy;
    }
    std::lock_guard<std::mutex> join_lock(p.join_mutex);
    std::array<std::shared_ptr<Impl::Mount>, max_mounts> workers{};
    {
        std::lock_guard<std::mutex> lock(p.mutex);
        p.stop = true;
        for (auto& item : p.mounts) {
            item.second->closing = true;
            item.second->stop = true;
            workers[item.second->slot] = item.second;
        }
        p.changed.notify_all();
    }
    for (const auto& state : workers) if (state) Impl::join(state);
    {
        std::unique_lock<std::mutex> lock(p.mutex);
        /* Concurrent Cancel/Finish callers can own detached callbacks. */
        p.changed.wait(lock, [&] { return p.requests.empty(); });
        p.mounts.clear();
    }
    return Error::ok;
}

Error Engine::health() const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->sticky_error;
}
} // namespace ppu_fs_aio
