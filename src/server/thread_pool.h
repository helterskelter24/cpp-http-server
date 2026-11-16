#pragma once

#include <condition_variable>
#include <cstddef>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

namespace hs {

// A fixed set of worker threads fed from a bounded FIFO of tasks.
//
// The queue is bounded and try_submit() never blocks, so the caller (the
// acceptor) learns immediately that the pool is saturated and can answer 503
// instead of stalling. `capacity` counts tasks waiting in the queue, not tasks
// already running on a worker.
//
// Tasks must not throw: an exception escaping a task ends the process, as it
// would from any std::thread. The caller wraps its work if it can fail.
class ThreadPool {
public:
    ThreadPool(size_t threads, size_t capacity);
    ~ThreadPool();  // calls shutdown()

    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;

    // False when the queue is full or the pool has been shut down; the task is
    // then destroyed without running.
    bool try_submit(std::function<void()> task);

    // Stops accepting tasks, lets the workers finish everything already queued,
    // then joins them. Idempotent; a second caller waits until the join is done.
    void shutdown();

    // Tasks waiting for a worker. Only the unit tests call it: it is how they
    // see that the queue bound holds without racing the workers.
    size_t queued() const;

private:
    void worker_loop();

    mutable std::mutex mu_;
    std::condition_variable has_work_;
    std::deque<std::function<void()>> queue_;
    size_t capacity_;
    bool stopping_ = false;

    // Separate from mu_ so workers can still take tasks while shutdown() waits
    // in join(); it only serializes callers of shutdown() itself.
    std::mutex join_mu_;
    std::vector<std::thread> workers_;
};

}  // namespace hs
