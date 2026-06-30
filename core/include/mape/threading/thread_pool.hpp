#ifndef MAPE_THREADING_THREAD_POOL_HPP
#define MAPE_THREADING_THREAD_POOL_HPP

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <queue>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace mape {

// A fixed-size worker pool with a mutex-guarded task queue (plan §5.2). Used to
// price a portfolio concurrently: enqueue one task per instrument, collect the
// futures. RAII — the destructor drains and joins all workers.
class ThreadPool {
public:
    explicit ThreadPool(unsigned n_threads = 0) {
        if (n_threads == 0)
            n_threads = std::max(1u, std::thread::hardware_concurrency());
        workers_.reserve(n_threads);
        for (unsigned i = 0; i < n_threads; ++i)
            workers_.emplace_back([this] { worker_loop(); });
    }

    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;

    ~ThreadPool() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stop_ = true;
        }
        cv_.notify_all();
        for (auto& w : workers_)
            if (w.joinable()) w.join();
    }

    // Submit work; returns a future for the result.
    template <typename F>
    auto submit(F&& f) -> std::future<std::invoke_result_t<F>> {
        using R = std::invoke_result_t<F>;
        auto task =
            std::make_shared<std::packaged_task<R()>>(std::forward<F>(f));
        std::future<R> fut = task->get_future();
        {
            std::lock_guard<std::mutex> lock(mutex_);
            tasks_.emplace([task] { (*task)(); });
        }
        cv_.notify_one();
        return fut;
    }

    std::size_t size() const noexcept { return workers_.size(); }

private:
    void worker_loop() {
        for (;;) {
            std::function<void()> task;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                cv_.wait(lock, [this] { return stop_ || !tasks_.empty(); });
                if (stop_ && tasks_.empty()) return;
                task = std::move(tasks_.front());
                tasks_.pop();
            }
            task();
        }
    }

    std::vector<std::thread> workers_;
    std::queue<std::function<void()>> tasks_;
    std::mutex mutex_;
    std::condition_variable cv_;
    bool stop_ = false;
};

}  // namespace mape

#endif  // MAPE_THREADING_THREAD_POOL_HPP
