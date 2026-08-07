#pragma once
#include <filesystem>
#include <functional>
#include <string>
#include <vector>
#include "hermes/llm/adapter.hpp"
#include "hermes/message.hpp"
#include "hermes/tool_registry.hpp"

namespace hermes {

// 供外部(server/WebUI)流式观察一次 run() 的回合级事件。
// 独立于 LlmEvent:后者是适配器层 wire 事件,这里表达"一次 agent 回合"的语义。
struct AgentEvent {
    enum class Type { TextDelta, ToolUse, ConfirmRequest, ToolResult, Done, Error, Close, Usage };
    Type type = Type::Done;
    std::string text;                // TextDelta
    ContentBlock tool_use;           // ToolUse / ConfirmRequest(含 id/name/input)
    std::string tool_id;             // ToolResult
    std::string tool_name;           // ToolResult
    bool tool_ok = false;            // ToolResult
    std::string tool_output;         // ToolResult
    std::string error;               // Error
    uint32_t usage_in = 0;           // Usage:本回合输入 token(最近一轮)
    uint32_t usage_out = 0;          // Usage:本回合累计输出 token
    uint32_t context_used = 0;       // Usage:上下文已用 ≈ 最近一轮 input+output
    uint32_t context_window = 0;     // Usage:provider 上下文窗口

    static AgentEvent make_text(std::string t) {
        AgentEvent e; e.type = Type::TextDelta; e.text = std::move(t); return e;
    }
    static AgentEvent make_tool_use(ContentBlock b) {
        AgentEvent e; e.type = Type::ToolUse; e.tool_use = std::move(b); return e;
    }
    static AgentEvent make_confirm_request(ContentBlock b) {
        AgentEvent e; e.type = Type::ConfirmRequest; e.tool_use = std::move(b); return e;
    }
    static AgentEvent make_tool_result(std::string id, std::string name, bool ok, std::string out) {
        AgentEvent e; e.type = Type::ToolResult;
        e.tool_id = std::move(id); e.tool_name = std::move(name);
        e.tool_ok = ok; e.tool_output = std::move(out);
        return e;
    }
    static AgentEvent make_done() { AgentEvent e; e.type = Type::Done; return e; }
    static AgentEvent make_usage(uint32_t in, uint32_t out, uint32_t used, uint32_t window) {
        AgentEvent e; e.type = Type::Usage;
        e.usage_in = in; e.usage_out = out; e.context_used = used; e.context_window = window;
        return e;
    }
    static AgentEvent make_err(std::string e2) {
        AgentEvent e; e.type = Type::Error; e.error = std::move(e2); return e;
    }
    static AgentEvent make_close() { AgentEvent e; e.type = Type::Close; return e; }
};
using AgentEventCallback = std::function<void(const AgentEvent&)>;

// 调度中枢:驱动「发消息→流式事件→若有 tool 调用则执行→结果回填→再问」的循环。
// 不关心 tool 怎么执行、UI 怎么展示;遇到需人工确认的调用先回调确认门禁。
class AgentLoop {
public:
    // workspace_root/user_skills_root 用于 system prompt(项目记忆 Agent.md + 技能清单);depth 为子代理嵌套层数。
    explicit AgentLoop(ToolRegistry& tools,
                       std::filesystem::path workspace_root = {},
                       std::filesystem::path user_skills_root = {},
                       int depth = 0)
        : tools_(tools), workspace_root_(std::move(workspace_root)),
          user_skills_root_(std::move(user_skills_root)), depth_(depth) {}

    // confirm_cb:需要确认的 tool 调用回调,返回是否放行(M1 CLI 给恒 true)。
    // on_event:回合级流式事件回调(SSE 转发用),默认空 → CLI 零改动。
    // 返回 false 表示过程中出现 LLM 错误(错误文本已写入 history)。
    bool run(const ProviderConfig& cfg, IProviderAdapter& llm,
             std::vector<Message>& history,
             std::function<bool(const ContentBlock&)> confirm_cb,
             AgentEventCallback on_event = AgentEventCallback{});

private:
    ToolRegistry& tools_;
    std::filesystem::path workspace_root_, user_skills_root_;
    int depth_ = 0;
};

} // namespace hermes
