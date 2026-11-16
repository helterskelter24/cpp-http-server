#include "server/thread_pool.h"

#include <utility>

namespace hs {

ThreadPool::ThreadPool(size_t threads, size_t capacity) : capacity_(capacity) {
    workers_.reserve(threads);
    try {
        for (size_t i = 0; i < threads; ++i) workers_.emplace_back([this] { worker_loop(); });
    } catch (...) {
        // std::thread throws std::system_error when the system cannot create
        // another thread. The destructor does not run for a constructor that
        // throws, and destroying a joinable std::thread calls std::terminate,
        // so the workers already started are stopped and joined here.
        shutdown();
        throw;
    }
}

ThreadPool::~ThreadPool() { shutdown(); }

bool ThreadPool::try_submit(std::function<void()> task) {
    {
        std::lock_guard<std::mutex> lock(mu_);
        if (stopping_ || queue_.size() >= capacity_) return false;
        queue_.push_back(std::move(task));
    }
    // Notifying after unlocking means the woken worker does not immediately
    // block on the mutex this thread still holds.
    has_work_.notify_one();
    return true;
}

void ThreadPool::shutdown() {
    {
        std::lock_guard<std::mutex> lock(mu_);
        stopping_ = true;
    }
    has_work_.notify_all();

    std::lock_guard<std::mutex> lock(join_mu_);
    for (std::thread& t : workers_)
        if (t.joinable()) t.join();
}

size_t ThreadPool::queued() const {
    std::lock_guard<std::mutex> lock(mu_);
    return queue_.size();
}

void ThreadPool::worker_loop() {
    for (;;) {
        std::function<void()> task;
        {
            std::unique_lock<std::mutex> lock(mu_);
            has_work_.wait(lock, [this] { return stopping_ || !queue_.empty(); });
            // Queued tasks still run after shutdown starts; a worker leaves
            // only once the queue is empty.
            if (queue_.empty()) return;
            task = std::move(queue_.front());
            queue_.pop_front();
        }
        task();  // run without the lock, so other workers keep taking tasks
    }
}

}  // namespace hs
