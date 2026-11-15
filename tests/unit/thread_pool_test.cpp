#include "server/thread_pool.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>

#include "test.h"

namespace {

// A one-shot gate: tasks block in wait() until open() is called. C++17 has no
// std::latch, and this is all the tests need.
class Gate {
public:
    void open() {
        std::lock_guard<std::mutex> lock(mu_);
        open_ = true;
        cv_.notify_all();
    }
    void wait() {
        std::unique_lock<std::mutex> lock(mu_);
        cv_.wait(lock, [this] { return open_; });
    }

private:
    std::mutex mu_;
    std::condition_variable cv_;
    bool open_ = false;
};

// Polls until `done` returns true or about two seconds pass.
template <typename Pred>
bool eventually(Pred done) {
    for (int i = 0; i < 2000; ++i) {
        if (done()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return done();
}

}  // namespace

TEST(thread_pool_runs_tasks) {
    std::atomic<int> count{0};
    hs::ThreadPool pool(4, 200);
    for (int i = 0; i < 100; ++i) CHECK(pool.try_submit([&count] { count.fetch_add(1); }));
    pool.shutdown();
    CHECK_EQ(count.load(), 100);
}

TEST(thread_pool_rejects_when_full) {
    Gate gate;
    std::atomic<bool> blocker_running{false};
    hs::ThreadPool pool(1, 2);

    // The single worker takes this task and blocks, so later tasks stay queued.
    CHECK(pool.try_submit([&] {
        blocker_running.store(true);
        gate.wait();
    }));
    CHECK(eventually([&] { return blocker_running.load(); }));

    // Capacity counts waiting tasks only: two fit, the third and later do not.
    CHECK(pool.try_submit([] {}));
    CHECK(pool.try_submit([] {}));
    CHECK_EQ(pool.queued(), 2u);
    CHECK(!pool.try_submit([] {}));
    CHECK(!pool.try_submit([] {}));
    CHECK_EQ(pool.queued(), 2u);

    gate.open();
    pool.shutdown();
    CHECK_EQ(pool.queued(), 0u);
}

TEST(thread_pool_shutdown_drains_queue) {
    Gate gate;
    std::atomic<bool> blocker_running{false};
    std::atomic<int> ran{0};
    hs::ThreadPool pool(1, 10);
    CHECK(pool.try_submit([&] {
        blocker_running.store(true);
        gate.wait();
    }));
    CHECK(eventually([&] { return blocker_running.load(); }));
    for (int i = 0; i < 5; ++i) CHECK(pool.try_submit([&ran] { ran.fetch_add(1); }));

    // shutdown() is called while the five tasks are still queued; it must run
    // them all before it returns.
    std::thread opener([&gate] {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        gate.open();
    });
    pool.shutdown();
    opener.join();
    CHECK_EQ(ran.load(), 5);
}

TEST(thread_pool_submit_after_shutdown_false) {
    std::atomic<int> ran{0};
    hs::ThreadPool pool(2, 10);
    pool.shutdown();
    CHECK(!pool.try_submit([&ran] { ran.fetch_add(1); }));
    CHECK_EQ(ran.load(), 0);
}

TEST(thread_pool_shutdown_idempotent) {
    std::atomic<int> ran{0};
    {
        hs::ThreadPool pool(2, 10);
        CHECK(pool.try_submit([&ran] { ran.fetch_add(1); }));
        pool.shutdown();
        pool.shutdown();
        // The destructor calls shutdown() a third time.
    }
    CHECK_EQ(ran.load(), 1);
}
