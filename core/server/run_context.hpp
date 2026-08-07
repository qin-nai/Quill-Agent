#pragma once
#include <atomic>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <httplib.h>
#include "hermes/agent_loop.hpp"
#include "blocking_queue.hpp"

namespace hermes {

// 一个运行中回合的共享状态:事件队列(worker→SSE)、confirm 应答通道、协作 abort。
struct RunContext {
    std::shared_ptr<BlockingQueue<AgentEvent>> queue = std::make_shared<BlockingQueue<AgentEvent>>();

    // confirm 应答通道:worker 线程阻塞等待 POST /confirm 或 stop。
    std::mutex mtx;
    std::condition_variable cv;
    bool answered = false;
    bool allow = false;
    std::string reason;

    // 协作取消:stop 或 SSE 连接关闭时置 true。
    std::atomic<bool> abort{false};

    // 供 confirm_cb 阻塞等待;abort 时按拒绝处理。
    bool wait_confirm() {
        std::unique_lock<std::mutex> lk(mtx);
        cv.wait(lk, [&] { return answered || abort.load(); });
        return !abort.load() && allow;
    }

    void answer(bool ok, std::string why) {
        {
            std::lock_guard<std::mutex> lk(mtx);
            answered = true;
            allow = ok;
            reason = std::move(why);
        }
        cv.notify_all();
    }

    void cancel() {
        abort.store(true);
        cv.notify_all();
    }
};
using RunContextPtr = std::shared_ptr<RunContext>;

// 把 AgentEvent 转成 SSE 帧写入 sink;返回 false 表示结束响应。
// 队列空时写 ": ping" 心跳防代理超时;看到 Close 或 abort 时结束。
std::function<bool(size_t, httplib::DataSink&)> make_sse_provider(const RunContextPtr& rc);

} // namespace hermes
