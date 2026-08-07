#pragma once
#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>

namespace hermes {

// 多生产者/单消费者阻塞队列。close() 后排空剩余元素,后续 pop 返回 false。
template <class T>
class BlockingQueue {
public:
    void push(T v) {
        {
            std::lock_guard<std::mutex> lk(mtx_);
            q_.push_back(std::move(v));
        }
        cv_.notify_one();
    }

    // 取到元素返回 true;超时或(已关闭且队列空)返回 false。
    bool pop(T& out, std::chrono::milliseconds timeout) {
        std::unique_lock<std::mutex> lk(mtx_);
        if (!cv_.wait_for(lk, timeout, [&] { return !q_.empty() || closed_; })) return false;
        if (q_.empty()) return false;  // 已关闭且排空
        out = std::move(q_.front());
        q_.pop_front();
        return true;
    }

    void close() {
        {
            std::lock_guard<std::mutex> lk(mtx_);
            closed_ = true;
        }
        cv_.notify_all();
    }

private:
    std::deque<T> q_;
    std::mutex mtx_;
    std::condition_variable cv_;
    bool closed_ = false;
};

} // namespace hermes
