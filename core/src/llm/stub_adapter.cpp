#include "hermes/llm/stub_adapter.hpp"

namespace hermes {

void StubAdapter::stream(const ProviderConfig&, const LlmRequest& req,
                         const EventCallback& on_event) {
    // 检测历史里是否已有 tool_result:有 → 已执行过工具,给最终回答
    bool already_called = false;
    for (const auto& m : req.messages)
        for (const auto& b : m.content)
            if (b.type == BlockType::ToolResult) already_called = true;

    if (!already_called) {
        on_event(LlmEvent{LlmEvent::Type::ToolUseComplete, {},
                          ContentBlock::make_tool_use("tu_001", "read_file", {{"path", "test.txt"}}), ""});
    } else {
        on_event(LlmEvent{LlmEvent::Type::TextDelta, "已读取文件,任务完成。", {}, ""});
    }
    on_event(LlmEvent{LlmEvent::Type::Done, {}, {}, ""});
}

} // namespace hermes
