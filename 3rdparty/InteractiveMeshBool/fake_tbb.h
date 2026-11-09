#pragma once
#include <condition_variable>
#include <future>
#include <thread>
#include <mutex>
#include <queue>
#include <vector>
#include <functional>
#include <atomic>

namespace
{
    class ThreadPool
    {
    public:
        explicit ThreadPool(size_t num_threads) : stop_(false)
        {
            for (size_t i = 0; i < num_threads; ++i)
            {
                workers_.emplace_back([this]() {
                    while (true)
                    {
                        std::function<void()> task;
                        {
                            std::unique_lock<std::mutex> lock(mutex_);
                            cv_.wait(lock, [this]() { return stop_ || !tasks_.empty(); });
                            if (stop_ && tasks_.empty()) return;
                            task = std::move(tasks_.front());
                            tasks_.pop();
                        }
                        task();
                    }
                    });
            }
        }

        ~ThreadPool()
        {
            {
                std::unique_lock<std::mutex> lock(mutex_);
                stop_ = true;
            }
            cv_.notify_all();
            for (auto& t : workers_) t.join();
        }

        void enqueue(std::function<void()> task)
        {
            {
                std::unique_lock<std::mutex> lock(mutex_);
                tasks_.push(std::move(task));
            }
            cv_.notify_one();
        }

        size_t size() const { return workers_.size(); }

    private:
        std::vector<std::thread> workers_;
        std::queue<std::function<void()>> tasks_;
        std::mutex mutex_;
        std::condition_variable cv_;
        bool stop_;
    };

    template<typename Index, typename Func>
    void parallel_for(Index first, Index last, Func func, size_t num_threads = 0)
    {
        if (first >= last) return;

        // osgVerse fix: the original implementation waited for "num_threads" tasks, but the
        // loop below only enqueues ceil(n / chunk_size) tasks, which is smaller than
        // num_threads whenever n is not a multiple of it (e.g. 12 items on a 16-thread CPU).
        // It therefore deadlocked for almost all small/medium meshes. Clamp the thread count
        // to the item count and wait for the number of tasks actually enqueued.
        const size_t n = static_cast<size_t>(last - first);
        if (num_threads == 0) num_threads = std::thread::hardware_concurrency();
        if (num_threads == 0) num_threads = 1;
        if (num_threads > n) num_threads = n;

        const size_t chunk_size = (n + num_threads - 1) / num_threads;
        const size_t num_tasks = (n + chunk_size - 1) / chunk_size;

        ThreadPool pool(num_tasks);
        std::atomic<size_t> completed{ 0 };
        std::mutex mutex;
        std::condition_variable cv;
        for (size_t t = 0; t < num_tasks; ++t)
        {
            Index chunk_begin = first + static_cast<Index>(t * chunk_size);
            Index chunk_end = std::min(chunk_begin + static_cast<Index>(chunk_size), last);
            if (chunk_begin >= chunk_end) break;

            pool.enqueue([=, &completed, &mutex, &cv]() {
                for (Index i = chunk_begin; i < chunk_end; ++i) func(i);
                if (++completed == num_tasks)
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    cv.notify_one();
                }
                });
        }

        std::unique_lock<std::mutex> lock(mutex);
        cv.wait(lock, [&]() { return completed == num_tasks; });
    }

    class TaskGroup
    {
    public:
        TaskGroup() : active_(0), stop_(false) {}
        ~TaskGroup() { wait(); }

        template<typename Func>
        void run(Func&& f)
        {
            {
                std::lock_guard<std::mutex> lock(mutex_);
                ++active_;
            }

            auto future = std::async(std::launch::async, [this, f]() mutable {
                try { f(); }
                catch (...)
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    exceptions_.push_back(std::current_exception());
                }

                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    if (--active_ == 0) { cv_.notify_all(); }
                }
            });

            std::lock_guard<std::mutex> lock(mutex_);
            futures_.push_back(std::move(future));
        }

        void wait()
        {
            std::unique_lock<std::mutex> lock(mutex_);
            cv_.wait(lock, [this]() { return active_ == 0; });

            lock.unlock();
            for (auto& f : futures_) { if (f.valid()) f.get(); }
            futures_.clear();

            lock.lock();
            if (!exceptions_.empty())
            {
                auto e = exceptions_.front();
                exceptions_.clear();
                std::rethrow_exception(e);
            }
        }

        bool is_empty() const
        {
            std::lock_guard<std::mutex> lock(mutex_);
            return active_ == 0;
        }

    private:
        std::atomic<size_t> active_;
        std::vector<std::future<void>> futures_;
        std::vector<std::exception_ptr> exceptions_;
        mutable std::mutex mutex_;
        std::condition_variable cv_;
        bool stop_;
    };

    class TaskGroupPool {
    public:
        explicit TaskGroupPool(size_t num_threads = 0)
            : pool_(num_threads == 0 ? std::thread::hardware_concurrency() : num_threads)
            , active_(0) {}

        template<typename Func>
        void run(Func&& f)
        {
            {
                std::lock_guard<std::mutex> lock(mutex_);
                ++active_;
            }

            pool_.enqueue([this, f]() mutable {
                try { f(); }
                catch (...)
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    exceptions_.push_back(std::current_exception());
                }

                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    if (--active_ == 0) { cv_.notify_all(); }
                }
            });
        }

        void wait()
        {
            std::unique_lock<std::mutex> lock(mutex_);
            cv_.wait(lock, [this]() { return active_ == 0; });

            if (!exceptions_.empty())
            {
                auto e = exceptions_.front();
                exceptions_.clear();
                std::rethrow_exception(e);
            }
        }

    private:
        ThreadPool pool_;
        std::atomic<size_t> active_;
        std::vector<std::exception_ptr> exceptions_;
        std::mutex mutex_;
        std::condition_variable cv_;
    };
}
