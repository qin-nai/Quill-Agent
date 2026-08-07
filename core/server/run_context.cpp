#include "run_context.hpp"
#include <nlohmann/json.hpp>

namespace hermes {

namespace {

using json = nlohmann::json;

// 把 tool_use 块转成 SSE 事件 data(不含 tool_result 的 output,输出在 tool_result 事件带)
json tool_use_json(const ContentBlock& b) {
    return json{{"id", b.tool_use_id}, {"name", b.tool_name}, {"input", b.tool_input}};
}

// 写一个完整 SSE 帧。返回 false 表示写入失败(客户端已断开)。
bool write_frame(httplib::DataSink& sink, const std::string& event, const json& data) {
    std::string frame = "event: " + event + "\ndata: " + data.dump() + "\n\n";
    return sink.write(frame.data(), frame.size());
}

} // namespace

std::function<bool(size_t, httplib::DataSink&)> make_sse_provider(const RunContextPtr& rc) {
    return [rc](size_t, httplib::DataSink& sink) -> bool {
        AgentEvent e;
        if (rc->queue->pop(e, std::chrono::milliseconds(15000))) {
            switch (e.type) {
                case AgentEvent::Type::TextDelta:
                    return write_frame(sink, "text_delta", json{{"text", e.text}});
                case AgentEvent::Type::ToolUse:
                    return write_frame(sink, "tool_use", tool_use_json(e.tool_use));
                case AgentEvent::Type::ConfirmRequest:
                    return write_frame(sink, "confirm_request", tool_use_json(e.tool_use));
                case AgentEvent::Type::ToolResult:
                    return write_frame(sink, "tool_result",
                                       json{{"id", e.tool_id}, {"name", e.tool_name},
                                            {"ok", e.tool_ok}, {"output", e.tool_output}});
                case AgentEvent::Type::Done:
                    return write_frame(sink, "done", json::object());
                case AgentEvent::Type::Usage:
                    return write_frame(sink, "usage",
                                       json{{"in", e.usage_in}, {"out", e.usage_out},
                                            {"context_used", e.context_used},
                                            {"context_window", e.context_window}});
                case AgentEvent::Type::Error:
                    return write_frame(sink, "error", json{{"message", e.error}});
                case AgentEvent::Type::Close:
                    rc->cancel();  // 通知 worker 停止,防客户端断连后线程泄漏
                    return false;  // 回合结束,关闭 SSE
            }
        }
        // 15s 无事件:心跳保活;若已 abort(被 stop)则结束。
        if (rc->abort.load()) return false;
        return sink.write(": ping\n\n", 8);
    };
}

} // namespace hermes
