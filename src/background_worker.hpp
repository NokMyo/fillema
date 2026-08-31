#pragma once

#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <optional>
#include <stop_token>
#include <thread>
#include <utility>

namespace fillema {

class TaskQueue final {
public:
    TaskQueue() : thread_([this](std::stop_token token) { run(token); }) {}
    ~TaskQueue() { stop(); }

    TaskQueue(const TaskQueue&) = delete;
    TaskQueue& operator=(const TaskQueue&) = delete;

    void submit(std::function<void()> task) {
        {
            std::lock_guard lock(mutex_);
            if (stopped_) return;
            tasks_.push_back(std::move(task));
        }
        condition_.notify_one();
    }

    void stop() {
        {
            std::lock_guard lock(mutex_);
            if (stopped_) return;
            stopped_ = true;
            tasks_.clear();
        }
        thread_.request_stop();
        condition_.notify_all();
        if (thread_.joinable()) thread_.join();
    }

private:
    void run(std::stop_token token) {
        while (!token.stop_requested()) {
            std::function<void()> task;
            {
                std::unique_lock lock(mutex_);
                condition_.wait(lock, [&] { return stopped_ || token.stop_requested() || !tasks_.empty(); });
                if (stopped_ || token.stop_requested()) return;
                task = std::move(tasks_.front());
                tasks_.pop_front();
            }
            try { task(); } catch (...) { /* Jobs report errors through their completion message. */ }
        }
    }

    std::mutex mutex_;
    std::condition_variable condition_;
    std::deque<std::function<void()>> tasks_;
    bool stopped_ = false;
    std::jthread thread_;
};

class LatestTaskWorker final {
public:
    LatestTaskWorker() : thread_([this](std::stop_token token) { run(token); }) {}
    ~LatestTaskWorker() { stop(); }

    LatestTaskWorker(const LatestTaskWorker&) = delete;
    LatestTaskWorker& operator=(const LatestTaskWorker&) = delete;

    void submit(std::function<void()> task) {
        {
            std::lock_guard lock(mutex_);
            if (stopped_) return;
            latest_ = std::move(task);
        }
        condition_.notify_one();
    }

    void stop() {
        {
            std::lock_guard lock(mutex_);
            if (stopped_) return;
            stopped_ = true;
            latest_.reset();
        }
        thread_.request_stop();
        condition_.notify_all();
        if (thread_.joinable()) thread_.join();
    }

private:
    void run(std::stop_token token) {
        while (!token.stop_requested()) {
            std::function<void()> task;
            {
                std::unique_lock lock(mutex_);
                condition_.wait(lock, [&] { return stopped_ || token.stop_requested() || latest_.has_value(); });
                if (stopped_ || token.stop_requested()) return;
                task = std::move(*latest_);
                latest_.reset();
            }
            try { task(); } catch (...) { /* Jobs report errors through their completion message. */ }
        }
    }

    std::mutex mutex_;
    std::condition_variable condition_;
    std::optional<std::function<void()>> latest_;
    bool stopped_ = false;
    std::jthread thread_;
};

} // namespace fillema

