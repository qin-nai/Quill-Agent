#include "hermes/agent_loop.hpp"
#include "hermes/system_prompt.hpp"
#include "hermes/task_runner.hpp"
#include <chrono>
#include <thread>

namespace hermes {

namespace {
constexpr int kMaxNesting = 3;  // 子代理最大嵌套层数

thread_local TaskRunner g_task_runner;
} // namespace

void set_task_runner(TaskRunner r) { g_task_runner = std::move(r); }
TaskRunner get_task_runner() { return g_task_runner; }

bool AgentLoop::run(const ProviderConfig& cfg, IProviderAdapter& llm,
                    std::vector<Message>& history,
                    std::function<bool(const ContentBlock&)> confirm_cb,
                    AgentEventCallback on_event) {
    // 子代理执行器:安装当前回合闭包,run 结束(含错误早退)恢复。
    const TaskRunner prev = get_task_runner();
    set_task_runner([this, &cfg, &llm, &confirm_cb, &on_event](const std::string& prompt) -> ToolResult {
        if (depth_ >= kMaxNesting)
            return {"错误:子任务嵌套超过 " + std::to_string(kMaxNesting) + " 层,拒绝执行", true};
        std::vector<Message> inner{{"user", {ContentBlock::make_text(prompt)}}};
        AgentLoop inner_loop(tools_, workspace_root_, user_skills_root_, depth_ + 1);
        inner_loop.run(cfg, llm, inner, confirm_cb, on_event);  // 同 cfg/llm/确认/事件
        std::string text;
        for (const auto& m : inner)
            if (m.role == "assistant")
                for (const auto& b : m.content)
                    if (b.type == BlockType::Text) text += b.text;
        return {text.empty() ? "(子任务未返回文本)" : text, false};
    });
    struct Restore {
        TaskRunner prev;
        ~Restore() { set_task_runner(std::move(prev)); }
    } restore{std::move(prev)};

    // 回合级 token 统计:累计输出,记录最近一轮输入/输出(近似上下文占用)。
    // 仅顶层回合发 usage/done:子代理(内层 run)也调 emit 会污染共享 SSE,让前端误判回合结束。
    uint64_t total_out = 0;
    uint32_t last_in = 0, last_out = 0;
    const auto emit_usage = [&]() {
        if (on_event && depth_ == 0)
            on_event(AgentEvent::make_usage(last_in, static_cast<uint32_t>(total_out),
                                            last_in + last_out, cfg.context_window));
    };

    while (true) {
        LlmRequest req;
        req.model = cfg.default_model;
        req.messages = history;
        req.tools = tools_.schemas();
        // 身份 + 项目记忆(Agent.md) + 技能清单,每轮构建(逻辑在 system_prompt 模块)
        req.system = build_system_prompt(workspace_root_, user_skills_root_);

        Message resp;
        resp.role = "assistant";
        bool has_tool_calls = false;
        bool errored = false;
        std::string err_text;

        // 每轮 LLM 调用带重试:仅当还没产出任何内容时重试(避免屏幕重复);
        // 4xx(429 限流除外)是确定性错误,重试无用;错误只在彻底放弃后上报一次。
        constexpr int kMaxRetry = 2;
        for (int attempt = 0;; ++attempt) {
            resp.content.clear();
            has_tool_calls = false;
            errored = false;
            err_text.clear();
            llm.stream(cfg, req, [&](const LlmEvent& ev) {
                switch (ev.type) {
                    case LlmEvent::Type::TextDelta:
                        if (!resp.content.empty() && resp.content.back().type == BlockType::Text) {
                            resp.content.back().text += ev.text;
                        } else {
                            resp.content.push_back(ContentBlock::make_text(ev.text));
                        }
                        if (on_event) on_event(AgentEvent::make_text(ev.text));
                        break;
                    case LlmEvent::Type::ToolUseComplete:
                        resp.content.push_back(ev.tool_use);
                        has_tool_calls = true;
                        if (on_event) on_event(AgentEvent::make_tool_use(ev.tool_use));
                        break;
                    case LlmEvent::Type::Error:
                        errored = true;
                        err_text = ev.error;  // 重试期间不转发给 UI,避免重复报错
                        break;
                    case LlmEvent::Type::Done:
                        last_in = ev.in_tokens;
                        last_out = ev.out_tokens;
                        total_out += ev.out_tokens;
                        break;
                }
            });
            if (!errored) break;
            if (!resp.content.empty()) break;               // 已产出内容,不重试
            if (err_text.find("aborted") != std::string::npos) break;  // 用户主动停止,不重试
            if (err_text.rfind("HTTP 4", 0) == 0 &&
                err_text.rfind("HTTP 429", 0) != 0) break;  // 4xx 非限流,重试无用
            if (attempt >= kMaxRetry) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(1000 * (attempt + 1)));
        }

        if (errored) {
            emit_usage();
            if (on_event) on_event(AgentEvent::make_err(err_text));
            if (!resp.content.empty() && resp.content.back().type == BlockType::Text) {
                resp.content.back().text += " [LLM 错误] " + err_text;
            } else {
                resp.content.push_back(ContentBlock::make_text("[LLM 错误] " + err_text));
            }
            history.push_back(std::move(resp));
            return false;
        }
        if (resp.content.empty()) break;  // 空响应,不把空 assistant 消息塞进历史

        history.push_back(resp);  // 拷贝入历史;resp 后面还要遍历,不能 move
        if (!has_tool_calls) break;  // 拿到最终文本回答,循环结束

        // 执行工具调用,结果回填为下一条 user 消息(tool_result 块)
        Message result_msg;
        result_msg.role = "user";
        for (const auto& call : resp.content) {
            if (call.type != BlockType::ToolUse) continue;

            ITool* tool = tools_.find(call.tool_name);
            ContentBlock rb = ContentBlock::make_tool_result(call.tool_use_id, "", false);
            const bool confirm_required = tool && tool->requires_confirmation();

            if (confirm_required && on_event) {
                // 阻塞等人工确认前,先让 UI 弹出确认请求
                on_event(AgentEvent::make_confirm_request(call));
            }

            if (!tool) {
                rb.tool_output = "未知 tool: " + call.tool_name;
                rb.is_error = true;
            } else if (confirm_required && confirm_cb && !confirm_cb(call)) {
                rb.tool_output = "用户拒绝执行该操作";
                rb.is_error = true;
            } else {
                auto res = tool->execute(call.tool_input);
                rb.tool_output = res.output;
                rb.is_error = res.is_error;
            }
            if (on_event) {
                on_event(AgentEvent::make_tool_result(call.tool_use_id, call.tool_name,
                                                      !rb.is_error, rb.tool_output));
            }
            result_msg.content.push_back(std::move(rb));
        }
        history.push_back(std::move(result_msg));
    }
    emit_usage();
    if (on_event && depth_ == 0) on_event(AgentEvent::make_done());  // 仅顶层回合发 done
    return true;
}

} // namespace hermes
