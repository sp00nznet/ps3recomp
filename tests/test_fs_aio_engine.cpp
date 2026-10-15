/* Standalone deterministic host tests. Compile this file together with
 * runtime/ppu/ppu_fs_aio_engine.cpp and the host threading library. No guest
 * memory, game data, filesystem operations or firmware execution is used. */
#include "../runtime/ppu/ppu_fs_aio_engine.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <future>
#include <mutex>
#include <set>
#include <stdexcept>
#include <thread>
#include <vector>

using namespace ppu_fs_aio;
using namespace std::chrono_literals;
#define CHECK(expression) do { if (!(expression)) { \
    std::fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #expression); std::abort(); \
} } while (0)

namespace {
class Event {
public:
    void set() { std::lock_guard<std::mutex> lock(mutex_); ready_ = true; changed_.notify_all(); }
    void wait() {
        std::unique_lock<std::mutex> lock(mutex_);
        CHECK(changed_.wait_for(lock, 5s, [&] { return ready_; }));
    }
private:
    std::mutex mutex_;
    std::condition_variable changed_;
    bool ready_ = false;
};

Work success() { return [] { return Result{ResultCode::success, 7, -12345}; }; }
Completion ignore() { return [](Id, const Result&) {}; }
PublishId publish() { return [](Id id) { CHECK(id >= 0 && id < 320); return true; }; }

void test_lifecycle_and_mounts() {
    std::atomic<unsigned> startup{0}, cleanup{0};
    const auto main_thread = std::this_thread::get_id();
    Engine engine({[&] { CHECK(std::this_thread::get_id() != main_thread); ++startup; return true; },
                   [&] { CHECK(std::this_thread::get_id() != main_thread); ++cleanup; }});
    CHECK(engine.submit("/disc", success(), ignore(), publish()).error == Error::not_initialized);
    for (const auto& mount : {"", "relative", "/", "/disc/", "/disc//x", "/disc/../x", "/disc/./x", "/disc\\x"})
        CHECK(engine.init(mount) == Error::invalid_mount);
    CHECK(engine.init("/disc") == Error::ok);
    CHECK(engine.init("/disc") == Error::ok);
    CHECK(startup == 1);
    for (unsigned i = 1; i < Engine::max_mounts; ++i)
        CHECK(engine.init("/mount" + std::to_string(i)) == Error::ok);
    CHECK(startup == 10);
    CHECK(engine.init("/overflow") == Error::capacity);
    CHECK(engine.submit("/disc", {}, ignore(), publish()).error == Error::invalid_request);
    CHECK(engine.submit("/disc", success(), {}, publish()).error == Error::invalid_request);
    CHECK(engine.submit("/disc", success(), ignore(), {}).error == Error::invalid_request);
    CHECK(engine.finish("/unknown") == Error::not_initialized);
    CHECK(engine.finish("/disc") == Error::ok);
    CHECK(cleanup == 1);
    CHECK(engine.init("/overflow") == Error::ok);
    CHECK(startup == 11);
    CHECK(engine.submit("/disc", success(), ignore(), publish()).error == Error::not_initialized);
    CHECK(engine.cancel(0) == Error::unknown_request);
    CHECK(engine.cancel(-1) == Error::invalid_request);
    CHECK(engine.cancel(320) == Error::invalid_request);
    CHECK(engine.shutdown() == Error::ok);
    CHECK(engine.shutdown() == Error::ok);
    CHECK(cleanup == 11);
    CHECK(engine.init("/later") == Error::stopping);
    CHECK(engine.submit("/overflow", success(), ignore(), publish()).error == Error::stopping);
}

void test_publication_and_detail() {
    Engine engine;
    CHECK(engine.init("/disc") == Error::ok);
    std::atomic<Id> published{-1};
    Event completed_event;
    std::atomic<unsigned> worked{0}, completed{0};
    const auto submitter = std::this_thread::get_id();
    auto work = [&] { CHECK(published.load() >= 0); ++worked; return Result{ResultCode::io_error, 123, -987654}; };
    auto completion = [&](Id id, const Result& result) {
        CHECK(std::this_thread::get_id() != submitter);
        CHECK(published.load() == id);
        CHECK(result.code == ResultCode::io_error && result.bytes == 123 && result.detail == -987654);
        ++completed;
        completed_event.set();
    };
    CHECK(engine.submit("/disc", work, completion, [](Id) { return false; }).error == Error::publication_failed);
    CHECK(engine.submit("/disc", work, completion, [](Id) -> bool { throw std::runtime_error("publish"); }).error == Error::publication_failed);
    CHECK(worked == 0 && completed == 0);
    const auto accepted = engine.submit("/disc", work, completion, [&](Id id) { published = id; return true; });
    CHECK(accepted.error == Error::ok && accepted.id == published.load());
    CHECK(accepted.id == 0);
    completed_event.wait();
    CHECK(engine.finish("/disc") == Error::ok);
    CHECK(worked == 1 && completed == 1);
}

void test_cancel_and_capacity_including_callback() {
    Engine engine;
    CHECK(engine.init("/disc") == Error::ok);
    Event callback_entered, release_callback;
    const auto cancelling_thread = std::this_thread::get_id();
    std::atomic<unsigned> completed{0}, cancelled{0}, second_work{0};
    const auto first = engine.submit("/disc", success(), [&](Id id, const Result&) {
        CHECK(engine.cancel(id) == Error::too_late);
        callback_entered.set();
        release_callback.wait();
        ++completed;
    }, publish());
    CHECK(first.error == Error::ok);
    callback_entered.wait();
    const auto second = engine.submit("/disc", [&] { ++second_work; return Result{}; },
        [&](Id, const Result& result) {
            CHECK(std::this_thread::get_id() == cancelling_thread);
            CHECK(result.code == ResultCode::cancelled && result.bytes == 0);
            CHECK(engine.finish("/disc") == Error::busy);
            CHECK(engine.shutdown() == Error::busy);
            ++cancelled; ++completed;
        }, publish());
    CHECK(second.error == Error::ok);
    CHECK(engine.cancel(second.id) == Error::ok);
    CHECK(engine.cancel(second.id) == Error::unknown_request);
    for (unsigned i = 1; i < Engine::max_outstanding_per_mount; ++i)
        CHECK(engine.submit("/disc", success(), [&](Id, const Result&) { ++completed; }, publish()).error == Error::ok);
    CHECK(engine.submit("/disc", success(), ignore(), publish()).error == Error::capacity);
    release_callback.set();
    CHECK(engine.finish("/disc") == Error::ok);
    CHECK(completed == 33 && cancelled == 1 && second_work == 0);
    CHECK(engine.cancel(first.id) == Error::unknown_request);
    CHECK(engine.cancel(second.id) == Error::unknown_request);
}

void test_executing_cancel_and_finish_drain() {
    Engine engine;
    CHECK(engine.init("/disc") == Error::ok);
    Event operation_entered, release_operation, finish_started;
    std::atomic<unsigned> callbacks{0};
    const auto first = engine.submit("/disc", [&] {
        operation_entered.set(); release_operation.wait(); return Result{ResultCode::success, 99};
    }, [&](Id, const Result& result) { CHECK(result.code == ResultCode::success); ++callbacks; }, publish());
    CHECK(first.error == Error::ok);
    operation_entered.wait();
    CHECK(engine.cancel(first.id) == Error::too_late);
    auto finishing = std::async(std::launch::async, [&] { finish_started.set(); return engine.finish("/disc"); });
    finish_started.wait();
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    for (;;) {
        const auto attempt = engine.submit("/disc", success(), ignore(), publish());
        if (attempt.error == Error::busy) break;
        CHECK(attempt.error == Error::ok || attempt.error == Error::capacity);
        CHECK(std::chrono::steady_clock::now() < deadline);
        std::this_thread::yield();
    }
    CHECK(engine.finish("/disc") == Error::busy);
    CHECK(finishing.wait_for(0s) != std::future_status::ready);
    release_operation.set();
    CHECK(finishing.wait_for(5s) == std::future_status::ready);
    CHECK(finishing.get() == Error::ok && callbacks == 1);
    CHECK(engine.init("/disc") == Error::ok);
}

void test_callback_reentry() {
    Engine engine;
    CHECK(engine.init("/disc") == Error::ok);
    Event reentered;
    std::atomic<unsigned> completions{0};
    CHECK(engine.submit("/disc", success(), [&](Id, const Result&) {
        CHECK(engine.finish("/disc") == Error::busy);
        CHECK(engine.shutdown() == Error::busy);
        CHECK(engine.submit("/disc", success(), [&](Id, const Result&) { ++completions; }, publish()).error == Error::ok);
        ++completions;
        reentered.set();
    }, publish()).error == Error::ok);
    reentered.wait();
    CHECK(engine.finish("/disc") == Error::ok);
    CHECK(completions == 2);
}

void test_finish_cancels_queued_on_caller() {
    Engine engine;
    CHECK(engine.init("/disc") == Error::ok);
    Event entered, release, cancelled;
    std::atomic<unsigned> queued_work{0}, completions{0};
    std::thread::id finisher;
    CHECK(engine.submit("/disc", [&] { entered.set(); release.wait(); return Result{}; },
        [&](Id, const Result& result) { CHECK(result.code == ResultCode::success); ++completions; }, publish()).error == Error::ok);
    entered.wait();
    CHECK(engine.submit("/disc", [&] { ++queued_work; return Result{}; }, [&](Id, const Result& result) {
        CHECK(std::this_thread::get_id() == finisher);
        CHECK(result.code == ResultCode::cancelled && result.bytes == 0);
        CHECK(engine.finish("/disc") == Error::busy);
        ++completions;
        cancelled.set();
    }, publish()).error == Error::ok);
    auto finishing = std::async(std::launch::async, [&] {
        finisher = std::this_thread::get_id();
        return engine.finish("/disc");
    });
    cancelled.wait();
    CHECK(queued_work == 0 && finishing.wait_for(0s) != std::future_status::ready);
    CHECK(engine.submit("/disc", success(), ignore(), publish()).error == Error::busy);
    release.set();
    CHECK(finishing.wait_for(5s) == std::future_status::ready);
    CHECK(finishing.get() == Error::ok && completions == 2 && queued_work == 0);
}

void test_concurrent_unique_ids() {
    Engine engine;
    CHECK(engine.init("/disc") == Error::ok);
    std::mutex ids_mutex;
    std::set<Id> ids;
    std::set<Id> seen;
    std::atomic<unsigned> completions{0};
    std::vector<std::thread> producers;
    for (unsigned producer = 0; producer < 4; ++producer) producers.emplace_back([&] {
        const auto deadline = std::chrono::steady_clock::now() + 5s;
        for (unsigned i = 0; i < 64;) {
            auto result = engine.submit("/disc", success(), [&](Id id, const Result& value) {
                CHECK((value.code == ResultCode::success && value.detail == -12345 && value.bytes == 7) ||
                      (value.code == ResultCode::cancelled && value.bytes == 0));
                std::lock_guard<std::mutex> lock(ids_mutex);
                CHECK(ids.erase(id) == 1); ++completions;
            }, [&](Id id) {
                CHECK(id >= 0 && id < 32);
                std::lock_guard<std::mutex> lock(ids_mutex);
                CHECK(ids.insert(id).second); seen.insert(id); return true;
            });
            if (result.error == Error::ok) ++i;
            else { CHECK(result.error == Error::capacity); std::this_thread::yield(); }
            CHECK(std::chrono::steady_clock::now() < deadline);
        }
    });
    for (auto& producer : producers) producer.join();
    CHECK(engine.finish("/disc") == Error::ok);
    CHECK(ids.empty() && !seen.empty() && seen.size() <= 32 && completions == 256);
}

void test_mount_slots_and_id_reuse() {
    Engine engine;
    for (unsigned i = 0; i < 10; ++i)
        CHECK(engine.init("/m" + std::to_string(i)) == Error::ok);
    Event entered, release, entered9, release9;
    const auto first = engine.submit("/m0", [&] { entered.set(); release.wait(); return Result{}; }, ignore(), publish());
    CHECK(first.error == Error::ok && first.id == 0);
    entered.wait();
    const auto first9 = engine.submit("/m9", [&] { entered9.set(); release9.wait(); return Result{}; }, ignore(), publish());
    CHECK(first9.error == Error::ok && first9.id == 288);
    entered9.wait();
    for (Id slot = 1; slot < 32; ++slot) {
        const auto item = engine.submit("/m9", success(), ignore(), publish());
        CHECK(item.error == Error::ok && item.id == 288 + slot);
    }
    CHECK(engine.submit("/m9", success(), ignore(), publish()).error == Error::capacity);
    CHECK(engine.cancel(319) == Error::ok);
    const auto reclaimed = engine.submit("/m9", success(), ignore(), publish());
    CHECK(reclaimed.error == Error::ok && reclaimed.id == 319);
    /* Exhausting m9 does not consume m1's request slots. */
    const auto independent = engine.submit("/m1", success(), ignore(), publish());
    CHECK(independent.error == Error::ok && independent.id == 32);
    release.set();
    release9.set();
    CHECK(engine.finish("/m9") == Error::ok);
    CHECK(engine.init("/replacement") == Error::ok);
    const auto reused = engine.submit("/replacement", success(), ignore(), publish());
    CHECK(reused.error == Error::ok && reused.id == 288);
    CHECK(engine.finish("/replacement") == Error::ok);
    CHECK(engine.shutdown() == Error::ok);
}

void test_independent_mount_workers() {
    std::atomic<unsigned> startups{0}, cleanups{0};
    std::mutex worker_mutex;
    std::set<std::thread::id> live_workers;
    Engine engine({[&] {
        std::lock_guard<std::mutex> lock(worker_mutex);
        CHECK(live_workers.insert(std::this_thread::get_id()).second);
        ++startups;
        return true;
    }, [&] {
        std::lock_guard<std::mutex> lock(worker_mutex);
        CHECK(live_workers.erase(std::this_thread::get_id()) == 1);
        ++cleanups;
    }});
    CHECK(engine.init("/a") == Error::ok);
    CHECK(engine.init("/b") == Error::ok);
    CHECK(engine.init("/a") == Error::ok && startups == 2);
    Event entered_a, release_a, completed_a, completed_b, again_a;
    std::thread::id worker_a;
    CHECK(engine.submit("/a", [&] {
        worker_a = std::this_thread::get_id();
        entered_a.set();
        release_a.wait();
        return Result{};
    }, [&](Id, const Result&) { completed_a.set(); }, publish()).error == Error::ok);
    entered_a.wait();
    CHECK(engine.submit("/b", [&] {
        CHECK(std::this_thread::get_id() != worker_a);
        /* Cross-mount joins from a worker are explicitly rejected. */
        CHECK(engine.finish("/a") == Error::busy);
        return Result{};
    }, [&](Id, const Result& result) {
        CHECK(result.code == ResultCode::success);
        completed_b.set();
    }, publish()).error == Error::ok);
    completed_b.wait(); // Must complete while a's operation is still blocked.
    auto finished_b = std::async(std::launch::async, [&] { return engine.finish("/b"); });
    CHECK(finished_b.wait_for(5s) == std::future_status::ready);
    CHECK(finished_b.get() == Error::ok && cleanups == 1);
    CHECK(startups == 2);
    release_a.set();
    completed_a.wait();
    CHECK(engine.submit("/a", [&] {
        CHECK(std::this_thread::get_id() == worker_a);
        return Result{};
    }, [&](Id, const Result& result) {
        CHECK(result.code == ResultCode::success);
        again_a.set();
    }, publish()).error == Error::ok);
    again_a.wait();
    CHECK(engine.finish("/a") == Error::ok && cleanups == 2);
    CHECK(engine.init("/fresh") == Error::ok && startups == 3);
    CHECK(engine.finish("/fresh") == Error::ok && cleanups == 3);
    CHECK(engine.shutdown() == Error::ok);
    CHECK(live_workers.empty());
}

void test_failures_and_shutdown() {
    for (bool throws : {false, true}) {
        std::atomic<unsigned> cleaned{0};
        Engine failed({[=]() -> bool { if (throws) throw std::runtime_error("startup"); return false; }, [&] { ++cleaned; }});
        CHECK(failed.init("/disc") == Error::startup_failed);
        CHECK(failed.init("/disc") == Error::startup_failed);
        CHECK(failed.submit("/disc", success(), ignore(), publish()).error == Error::not_initialized);
        CHECK(failed.shutdown() == Error::ok && cleaned == 2);
    }
    Engine errors;
    CHECK(errors.init("/disc") == Error::ok);
    std::atomic<unsigned> exception_completions{0};
    Event work_exception_done, completion_exception_done;
    CHECK(errors.submit("/disc", []() -> Result { throw std::runtime_error("work"); },
        [&](Id, const Result& result) { CHECK(result.code == ResultCode::engine_failure && result.bytes == 0); ++exception_completions; work_exception_done.set(); }, publish()).error == Error::ok);
    CHECK(errors.submit("/disc", success(), [&](Id, const Result&) { completion_exception_done.set(); throw std::runtime_error("completion"); }, publish()).error == Error::ok);
    work_exception_done.wait();
    completion_exception_done.wait();
    CHECK(errors.finish("/disc") == Error::ok);
    CHECK(exception_completions == 1 && errors.health() == Error::engine_failure);
    Engine cleanup_error({{}, [] { throw std::runtime_error("cleanup"); }});
    CHECK(cleanup_error.init("/disc") == Error::ok);
    CHECK(cleanup_error.shutdown() == Error::ok);
    CHECK(cleanup_error.health() == Error::engine_failure);

    Event work_entered, release_work, shutdown_started;
    Engine draining;
    CHECK(draining.init("/disc") == Error::ok);
    std::atomic<unsigned> drained{0};
    CHECK(draining.submit("/disc", [&] { work_entered.set(); release_work.wait(); return Result{}; },
        [&](Id, const Result&) { ++drained; }, publish()).error == Error::ok);
    work_entered.wait();
    auto shutting = std::async(std::launch::async, [&] { shutdown_started.set(); return draining.shutdown(); });
    shutdown_started.wait();
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (draining.submit("/disc", success(), [&](Id, const Result&) { ++drained; }, publish()).error != Error::stopping) {
        CHECK(std::chrono::steady_clock::now() < deadline); std::this_thread::yield();
    }
    CHECK(shutting.wait_for(0s) != std::future_status::ready);
    release_work.set();
    CHECK(shutting.wait_for(5s) == std::future_status::ready);
    CHECK(shutting.get() == Error::ok && drained >= 1);
    const auto stopped_count = drained.load();
    CHECK(draining.shutdown() == Error::ok && drained == stopped_count);

    std::atomic<unsigned> destroyed_callbacks{0}, destroyed_cleanup{0};
    {
        Engine automatic({{}, [&] { ++destroyed_cleanup; }});
        CHECK(automatic.init("/disc") == Error::ok);
        for (unsigned i = 0; i < 16; ++i)
            CHECK(automatic.submit("/disc", success(), [&](Id, const Result&) { ++destroyed_callbacks; }, publish()).error == Error::ok);
    }
    CHECK(destroyed_callbacks == 16 && destroyed_cleanup == 1);
}
}

int main() {
    test_lifecycle_and_mounts();
    test_publication_and_detail();
    test_cancel_and_capacity_including_callback();
    test_executing_cancel_and_finish_drain();
    test_callback_reentry();
    test_finish_cancels_queued_on_caller();
    test_concurrent_unique_ids();
    test_mount_slots_and_id_reuse();
    test_independent_mount_workers();
    test_failures_and_shutdown();
    std::puts("AIO engine host tests passed");
    return 0;
}
