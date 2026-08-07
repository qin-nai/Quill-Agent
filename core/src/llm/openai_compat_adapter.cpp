#include "hermes/llm/openai_compat_adapter.hpp"
#include "sse_reader.hpp"
#include <map>
#include <nlohmann/json.hpp>

namespace hermes {

namespace {
// 内部 user 消息(含图片)→ chat completions 的 content(字符串或多模态数组)
nlohmann::json build_user_content(const std::string& text,
                                  const std::vector<ContentBlock>& images) {
    if (images.empty()) return text;
    nlohmann::json parts = nlohmann::json::array();
    if (!text.empty()) parts.push_back({{"type", "text"}, {"text", text}});
    for (const auto& img : images) {
        parts.push_back({{"type", "image_url"},
                         {"image_url",
                          {{"url", "data:" + img.image_media_type + ";base64," + img.image_base64}}}});
    }
    return parts;
}
} // namespace

void OpenAICompatAdapter::stream(const ProviderConfig& cfg, const LlmRequest& req,
                                 const EventCallback& on_event) {
    // ── 请求体:chat completions ──
    nlohmann::json body;
    body["model"] = req.model.empty() ? cfg.default_model : req.model;
    body["stream"] = true;
    if (!req.tools.empty()) {
        nlohmann::json tools = nlohmann::json::array();
        for (const auto& t : req.tools)
            tools.push_back({{"type", "function"},
                             {"function", {{"name", t.name},
                                           {"description", t.description},
                                           {"parameters", t.input_schema}}}});
        body["tools"] = tools;
    }

    nlohmann::json messages = nlohmann::json::array();
    if (!req.system.empty()) {
        messages.push_back({{"role", "system"}, {"content", req.system}});
    }

    for (const auto& m : req.messages) {
        if (m.role == "system") {
            std::string text;
            for (const auto& b : m.content) text += b.text;
            messages.push_back({{"role", "system"}, {"content", text}});
            continue;
        }

        if (m.role == "assistant") {
            nlohmann::json assistant;
            assistant["role"] = "assistant";
            std::string text;
            nlohmann::json tool_calls = nlohmann::json::array();
            for (const auto& b : m.content) {
                if (b.type == BlockType::Text) {
                    text += b.text;
                } else if (b.type == BlockType::ToolUse) {
                    tool_calls.push_back({{"id", b.tool_use_id}, {"type", "function"},
                                          {"function", {{"name", b.tool_name},
                                                        {"arguments", b.tool_input.is_null()
                                                                         ? std::string()
                                                                         : b.tool_input.dump()}}}});
                }
            }
            assistant["content"] = text.empty() ? nlohmann::json(nullptr) : nlohmann::json(text);
            if (!tool_calls.empty()) assistant["tool_calls"] = tool_calls;
            messages.push_back(std::move(assistant));
            continue;
        }

        // role == "user":拆分 tool_result → role:"tool" 消息;其余 → role:"user"
        bool has_tool_results = false;
        std::string text;
        std::vector<ContentBlock> images;
        nlohmann::json tool_msgs = nlohmann::json::array();
        for (const auto& b : m.content) {
            if (b.type == BlockType::ToolResult) {
                has_tool_results = true;
                tool_msgs.push_back({{"role", "tool"},
                                     {"tool_call_id", b.tool_use_id},
                                     {"content", b.is_error ? "[tool error] " + b.tool_output
                                                            : b.tool_output}});
            } else if (b.type == BlockType::Text) {
                text += b.text;
            } else if (b.type == BlockType::Image) {
                images.push_back(b);
            }
        }

        if (has_tool_results) {
            // OpenAI 要求 tool 结果紧跟对应 assistant tool_calls 消息
            for (auto& tm : tool_msgs) messages.push_back(std::move(tm));
            // 同一条内部消息里混排的 text/图片放最后,作为新的一轮 user 消息
            if (!text.empty() || !images.empty())
                messages.push_back({{"role", "user"},
                                    {"content", build_user_content(text, images)}});
        } else {
            messages.push_back({{"role", "user"}, {"content", build_user_content(text, images)}});
        }
    }
    body["messages"] = messages;

    HttpRequest hr;
    hr.method = "POST";
    hr.url = cfg.base_url + "/chat/completions";
    hr.headers = {{"content-type", "application/json"},
                  {"Authorization", "Bearer " + cfg.api_key_ref}};  // M2:从 api_key_ref 解析
    hr.body = body.dump();

    // ── SSE 解析:tool 参数按 index 分组累计 ──
    struct ToolAccum { std::string id, name, args; };
    std::map<int, ToolAccum> tool_accs;
    bool done_emitted = false;
    std::string error_body;

    auto finish = [&]() {
        if (done_emitted) return;
        for (auto& [idx, acc] : tool_accs) {
            nlohmann::json input = nlohmann::json::object();
            if (!acc.args.empty()) {
                try { input = nlohmann::json::parse(acc.args); }
                catch (...) { input = nlohmann::json::object(); }
            }
            on_event(LlmEvent{LlmEvent::Type::ToolUseComplete, {},
                              ContentBlock::make_tool_use(acc.id, acc.name, input), ""});
        }
        done_emitted = true;
        on_event(LlmEvent{LlmEvent::Type::Done, {}, {}, ""});
    };

    auto handle_sse = [&](const std::string& event, const std::string& data) {
        if (data == "[DONE]") { finish(); return; }
        nlohmann::json j;
        try { j = nlohmann::json::parse(data); }
        catch (...) { return; }

        if (!j.contains("choices")) return;
        const auto& choice = j["choices"][0];

        if (choice.contains("delta")) {
            const auto& d = choice["delta"];
            if (d.contains("content") && d["content"].is_string()) {
                on_event(LlmEvent{LlmEvent::Type::TextDelta, d["content"].get<std::string>(), {}, ""});
            }
            // reasoning_content(R1/QwQ 等)MVP 忽略
            if (d.contains("tool_calls")) {
                for (const auto& tc : d["tool_calls"]) {
                    const int idx = tc.value("index", 0);
                    auto& acc = tool_accs[idx];
                    if (tc.contains("id")) acc.id = tc["id"].get<std::string>();
                    if (tc.contains("function") && tc["function"].is_object()) {
                        const auto& fn = tc["function"];
                        if (fn.contains("name")) acc.name = fn["name"].get<std::string>();
                        if (fn.contains("arguments")) acc.args += fn["arguments"].get<std::string>();
                    }
                }
            }
        }
        if (choice.contains("finish_reason") && !choice["finish_reason"].is_null()) {
            finish();  // "tool_calls" / "stop"
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
        finish();
    }
}

} // namespace hermes
