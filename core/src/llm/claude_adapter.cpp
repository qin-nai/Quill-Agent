#include "hermes/llm/claude_adapter.hpp"
#include "sse_reader.hpp"
#include <nlohmann/json.hpp>

namespace hermes {

namespace {
// 内部 ContentBlock → Claude content block JSON
nlohmann::json to_claude_block(const ContentBlock& b) {
    switch (b.type) {
        case BlockType::Text:
            return {{"type", "text"}, {"text", b.text}};
        case BlockType::Image:
            return {{"type", "image"},
                    {"source", {{"type", "base64"},
                                {"media_type", b.image_media_type},
                                {"data", b.image_base64}}}};
        case BlockType::ToolUse:
            return {{"type", "tool_use"},
                    {"id", b.tool_use_id},
                    {"name", b.tool_name},
                    {"input", b.tool_input.is_null() ? nlohmann::json::object() : b.tool_input}};
        case BlockType::ToolResult:
            return {{"type", "tool_result"},
                    {"tool_use_id", b.tool_use_id},
                    {"content", b.tool_output},
                    {"is_error", b.is_error}};
    }
    return nullptr;
}
} // namespace

void ClaudeAdapter::stream(const ProviderConfig& cfg, const LlmRequest& req,
                           const EventCallback& on_event) {
    // ── 请求体:Claude 原生 1:1 ──
    nlohmann::json body;
    body["model"] = req.model.empty() ? cfg.default_model : req.model;
    body["max_tokens"] = 16000;
    body["stream"] = true;
    if (cfg.disable_thinking) body["thinking"] = {{"type", "disabled"}};  // 默认开思考的端点关掉,否则文本被占用
    if (!req.system.empty()) body["system"] = req.system;

    nlohmann::json messages = nlohmann::json::array();
    for (const auto& m : req.messages) {
        if (m.role == "system") continue;  // system 走顶层字段
        nlohmann::json blocks = nlohmann::json::array();
        for (const auto& b : m.content) blocks.push_back(to_claude_block(b));
        messages.push_back({{"role", m.role}, {"content", blocks}});
    }
    body["messages"] = messages;

    if (!req.tools.empty()) {
        nlohmann::json tools = nlohmann::json::array();
        for (const auto& t : req.tools)
            tools.push_back({{"name", t.name},
                             {"description", t.description},
                             {"input_schema", t.input_schema}});
        body["tools"] = tools;
    }

    HttpRequest hr;
    hr.method = "POST";
    hr.url = cfg.base_url + "/v1/messages";
    hr.headers = {{"content-type", "application/json"},
                  {"x-api-key", cfg.api_key_ref},  // M2:从 api_key_ref 解析真实 key
                  {"anthropic-version", "2023-06-01"}};
    hr.body = body.dump();

    // ── SSE 解析状态 ──
    std::string cur_tool_id, cur_tool_name, cur_tool_args;
    uint32_t in_tokens = 0, out_tokens = 0;
    bool done_emitted = false;
    std::string error_body;

    auto handle_sse = [&](const std::string& event, const std::string& data) {
        nlohmann::json j;
        try { j = nlohmann::json::parse(data); }
        catch (...) { return; }

        if (event == "message_start") {
            in_tokens = j.value("message", nlohmann::json::object())
                            .value("usage", nlohmann::json::object())
                            .value("input_tokens", 0);
        } else if (event == "message_delta") {
            out_tokens = j.value("usage", nlohmann::json::object())
                             .value("output_tokens", 0);  // 输出累计
        } else if (event == "content_block_start") {
            const auto& cb = j.value("content_block", nlohmann::json::object());
            if (cb.value("type", "") == "tool_use") {
                cur_tool_id = cb.value("id", "");
                cur_tool_name = cb.value("name", "");
                cur_tool_args.clear();
            }
        } else if (event == "content_block_delta") {
            const auto& d = j.value("delta", nlohmann::json::object());
            if (d.value("type", "") == "text_delta") {
                on_event(LlmEvent{LlmEvent::Type::TextDelta, d.value("text", ""), {}, ""});
            } else if (d.value("type", "") == "input_json_delta") {
                cur_tool_args += d.value("partial_json", "");
            }
            // thinking_delta 等忽略(MVP)
        } else if (event == "content_block_stop") {
            if (!cur_tool_name.empty()) {
                nlohmann::json input = nlohmann::json::object();
                if (!cur_tool_args.empty()) {
                    try { input = nlohmann::json::parse(cur_tool_args); }
                    catch (...) { input = nlohmann::json::object(); }
                }
                on_event(LlmEvent{LlmEvent::Type::ToolUseComplete, {},
                                  ContentBlock::make_tool_use(cur_tool_id, cur_tool_name, input), ""});
                cur_tool_id.clear();
                cur_tool_name.clear();
                cur_tool_args.clear();
            }
        } else if (event == "message_stop") {
            done_emitted = true;
            on_event(LlmEvent{LlmEvent::Type::Done, {}, {}, "", in_tokens, out_tokens});
        }
    };

    detail::SseReader reader(handle_sse);
    auto resp = transport_(hr, [&](const std::string& chunk) {
        if (error_body.size() < 4096) error_body += chunk;
        reader.feed(chunk);
    });
    reader.flush();

    if (resp.status != 200) {
        on_event(LlmEvent{LlmEvent::Type::Error, {}, {},
                          "HTTP " + std::to_string(resp.status) +
                              (error_body.empty() ? "" : ": " + error_body)});
    } else if (!done_emitted && !resp.stream_complete) {
        // 传输中途被超时/连接中断 → 不能假装正常结束,必须报错
        on_event(LlmEvent{LlmEvent::Type::Error, {}, {},
                          "响应中断: " + (resp.error.empty() ? "连接被断开" : resp.error)});
    } else if (!done_emitted) {
        on_event(LlmEvent{LlmEvent::Type::Done, {}, {}, "", in_tokens, out_tokens});
    }
}

} // namespace hermes
