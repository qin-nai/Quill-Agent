#pragma once
#include <functional>
#include <string>
#include <vector>
#include "hermes/message.hpp"
#include "hermes/provider.hpp"
#include "hermes/tool.hpp"

namespace hermes {

struct LlmRequest {
    std::string model;
    std::vector<Message> messages;  // 内部消息模型
    std::vector<ToolSchema> tools;
    std::string system;             // 顶层 system;Claude 走 system 字段,OpenAI 走首条 system 消息
};

// 适配器流式产出的增量事件,Agent Loop 负责组装成完整 Message。
struct LlmEvent {
    enum class Type { TextDelta, ToolUseComplete, Done, Error };
    Type type = Type::Done;
    std::string text;       // TextDelta:文本增量
    ContentBlock tool_use;  // ToolUseComplete:完整 tool 调用(含已拼好的参数 JSON)
    std::string error;      // Error
    uint32_t in_tokens = 0;   // Done:本次请求输入 token(含历史)
    uint32_t out_tokens = 0;  // Done:本次请求输出 token(累计)
};

using EventCallback = std::function<void(const LlmEvent&)>;

// 所有 provider 适配器实现这个接口。内部消息模型保持 Claude 风格 content block,
// 适配器负责双向翻译 + SSE 增量解析。M2 通过注入的 HttpTransport 接真实网络。
class IProviderAdapter {
public:
    virtual ~IProviderAdapter() = default;
    virtual void stream(const ProviderConfig& cfg, const LlmRequest& req,
                        const EventCallback& on_event) = 0;
};

} // namespace hermes
