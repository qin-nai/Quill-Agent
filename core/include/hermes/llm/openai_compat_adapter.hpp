#pragma once
#include "hermes/http_transport.hpp"
#include "hermes/llm/adapter.hpp"

namespace hermes {

// OpenAI chat completions 兼容协议,覆盖 OpenAI/DeepSeek/Qwen/Kimi/GLM/Ollama/聚合。
// 内部 Claude 风格消息 → chat completions 双向翻译:
//   - ToolUse  → assistant.tool_calls
//   - ToolResult → role:"tool" 消息(必须紧跟对应 assistant tool_calls)
// SSE 中 tool 参数按 index 分组累计成完整 JSON 后再 parse。
class OpenAICompatAdapter : public IProviderAdapter {
public:
    explicit OpenAICompatAdapter(HttpTransport transport) : transport_(std::move(transport)) {}
    void stream(const ProviderConfig& cfg, const LlmRequest& req,
                const EventCallback& on_event) override;

private:
    HttpTransport transport_;
};

} // namespace hermes
