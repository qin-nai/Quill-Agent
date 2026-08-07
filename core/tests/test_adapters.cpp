// 适配器单元测试:用假 transport 回放 canned SSE,
// 验证 claude / openai_compat 两个适配器的请求翻译与流式解析。
#include <iostream>
#include <string>
#include <vector>
#include "hermes/llm/claude_adapter.hpp"
#include "hermes/llm/openai_compat_adapter.hpp"
#include "hermes/message.hpp"
#include "hermes/provider.hpp"
#include "hermes/tool.hpp"
#include <nlohmann/json.hpp>

using namespace hermes;

static int g_failures = 0;
#define CHECK(cond)                                                      \
    do {                                                                 \
        if (!(cond)) {                                                   \
            ++g_failures;                                                \
            std::cerr << "FAIL line " << __LINE__ << ": " #cond "\n";   \
        }                                                                \
    } while (0)

// 回放固定 SSE 的假 transport,并捕获发出的请求
struct FakeTransport {
    HttpRequest captured;
    std::string sse_body;
    int split_at = 0;  // 把 body 切成两块喂给解析器,测 chunk 边界
    int status = 200;

    HttpTransport make() {
        return [this](const HttpRequest& req, const BodyChunkCallback& cb) {
            captured = req;
            if (split_at > 0 && split_at < static_cast<int>(sse_body.size())) {
                cb(sse_body.substr(0, split_at));
                cb(sse_body.substr(split_at));
            } else {
                cb(sse_body);
            }
            HttpResponse r;
            r.status = status;
            return r;
        };
    }
};

static std::vector<LlmEvent> collect(IProviderAdapter& adapter, const ProviderConfig& cfg,
                                     const LlmRequest& req) {
    std::vector<LlmEvent> events;
    adapter.stream(cfg, req, [&](const LlmEvent& e) { events.push_back(e); });
    return events;
}

static LlmRequest make_req() {
    LlmRequest req;
    req.model = "test-model";
    return req;
}

// 带一轮完整工具往返的内部消息(用于验证请求翻译)
static LlmRequest make_roundtrip_req() {
    LlmRequest req;
    req.model = "test-model";
    req.tools = {{"read_file", "读取文件",
                  {{"type", "object"},
                   {"properties", {{"path", {{"type", "string"}}}}},
                   {"required", {"path"}}}}};

    Message user;
    user.role = "user";
    user.content = {ContentBlock::make_text("看下 test.txt")};
    Message assistant;
    assistant.role = "assistant";
    assistant.content = {ContentBlock::make_tool_use("call_1", "read_file", {{"path", "test.txt"}})};
    Message tooluser;
    tooluser.role = "user";
    tooluser.content = {ContentBlock::make_tool_result("call_1", "文件内容...")};
    req.messages = {user, assistant, tooluser};
    return req;
}

static void test_openai_text() {
    FakeTransport ft;
    ft.sse_body =
        "data: {\"choices\":[{\"delta\":{\"role\":\"assistant\"}}]}\n\n"
        "data: {\"choices\":[{\"delta\":{\"content\":\"你好\"}}]}\n\n"
        "data: {\"choices\":[{\"delta\":{\"content\":\",世界\"}}]}\n\n"
        "data: {\"choices\":[{\"delta\":{},\"finish_reason\":\"stop\"}]}\n\n"
        "data: [DONE]\n\n";
    ft.split_at = 40;  // 故意断在行中间

    OpenAICompatAdapter adapter(ft.make());
    ProviderConfig cfg;
    cfg.adapter = AdapterType::OpenAICompat;
    cfg.base_url = "https://example.com/v1";
    cfg.default_model = "m";

    auto events = collect(adapter, cfg, make_req());
    CHECK(events.size() == 3);
    CHECK(events[0].type == LlmEvent::Type::TextDelta);
    CHECK(events[0].text == "你好");
    CHECK(events[1].type == LlmEvent::Type::TextDelta);
    CHECK(events[1].text == ",世界");
    CHECK(events[2].type == LlmEvent::Type::Done);
    CHECK(ft.captured.url == "https://example.com/v1/chat/completions");
}

static void test_openai_tool() {
    FakeTransport ft;
    ft.sse_body =
        "data: {\"choices\":[{\"delta\":{\"role\":\"assistant\",\"content\":null}}]}\n\n"
        "data: {\"choices\":[{\"delta\":{\"tool_calls\":[{\"index\":0,\"id\":\"call_1\","
        "\"type\":\"function\",\"function\":{\"name\":\"read_file\",\"arguments\":\"\"}}]}}]}\n\n"
        "data: {\"choices\":[{\"delta\":{\"tool_calls\":[{\"index\":0,"
        "\"function\":{\"arguments\":\"{\\\"path\\\":\\\"\"}}]}}]}\n\n"
        "data: {\"choices\":[{\"delta\":{\"tool_calls\":[{\"index\":0,"
        "\"function\":{\"arguments\":\"test.txt\\\"}\"}}]}}]}\n\n"
        "data: {\"choices\":[{\"delta\":{},\"finish_reason\":\"tool_calls\"}]}\n\n"
        "data: [DONE]\n\n";
    ft.split_at = 80;

    OpenAICompatAdapter adapter(ft.make());
    ProviderConfig cfg;
    cfg.adapter = AdapterType::OpenAICompat;
    cfg.base_url = "https://example.com/v1";
    cfg.default_model = "m";

    auto events = collect(adapter, cfg, make_req());
    CHECK(events.size() == 2);
    CHECK(events[0].type == LlmEvent::Type::ToolUseComplete);
    CHECK(events[0].tool_use.tool_use_id == "call_1");
    CHECK(events[0].tool_use.tool_name == "read_file");
    CHECK(events[0].tool_use.tool_input.value("path", "") == "test.txt");  // 跨片段重组成功
    CHECK(events[1].type == LlmEvent::Type::Done);
}

static void test_openai_translation() {
    FakeTransport ft;
    ft.sse_body = "data: [DONE]\n\n";  // 空响应即可,只看请求形状

    OpenAICompatAdapter adapter(ft.make());
    ProviderConfig cfg;
    cfg.adapter = AdapterType::OpenAICompat;
    cfg.base_url = "https://example.com/v1";
    cfg.default_model = "m";
    cfg.api_key_ref = "my-key";

    collect(adapter, cfg, make_roundtrip_req());

    const auto body = nlohmann::json::parse(ft.captured.body);
    CHECK(body["model"] == "test-model");
    CHECK(body["stream"] == true);
    CHECK(body["tools"][0]["function"]["name"] == "read_file");

    const auto& msgs = body["messages"];
    CHECK(msgs.size() == 3);
    CHECK(msgs[0]["role"] == "user");
    CHECK(msgs[1]["role"] == "assistant");
    CHECK(msgs[1]["tool_calls"][0]["function"]["name"] == "read_file");
    CHECK(msgs[1]["tool_calls"][0]["function"]["arguments"] == "{\"path\":\"test.txt\"}");
    CHECK(msgs[2]["role"] == "tool");  // tool_result 翻译成 role:"tool"
    CHECK(msgs[2]["tool_call_id"] == "call_1");
    CHECK(msgs[2]["content"] == "文件内容...");

    bool has_bearer = false;
    for (const auto& [k, v] : ft.captured.headers)
        if (k == "Authorization") has_bearer = (v == "Bearer my-key");
    CHECK(has_bearer);
}

static void test_claude_tool() {
    FakeTransport ft;
    ft.sse_body =
        "event: content_block_start\n"
        "data: {\"type\":\"content_block_start\",\"index\":0,"
        "\"content_block\":{\"type\":\"tool_use\",\"id\":\"tu_001\",\"name\":\"read_file\",\"input\":{}}}\n\n"
        "event: content_block_delta\n"
        "data: {\"type\":\"content_block_delta\",\"index\":0,"
        "\"delta\":{\"type\":\"input_json_delta\",\"partial_json\":\"{\\\"path\\\":\"}}\n\n"
        "event: content_block_delta\n"
        "data: {\"type\":\"content_block_delta\",\"index\":0,"
        "\"delta\":{\"type\":\"input_json_delta\",\"partial_json\":\"\\\"test.txt\\\"}\"}}\n\n"
        "event: content_block_stop\n"
        "data: {\"type\":\"content_block_stop\",\"index\":0}\n\n"
        "event: message_delta\n"
        "data: {\"type\":\"message_delta\",\"delta\":{\"stop_reason\":\"tool_use\"},\"usage\":{\"output_tokens\":10}}\n\n"
        "event: message_stop\n"
        "data: {\"type\":\"message_stop\"}\n\n";
    ft.split_at = 120;

    ClaudeAdapter adapter(ft.make());
    ProviderConfig cfg;
    cfg.adapter = AdapterType::Claude;
    cfg.base_url = "https://example.com";
    cfg.default_model = "m";

    auto events = collect(adapter, cfg, make_req());
    CHECK(events.size() == 2);
    CHECK(events[0].type == LlmEvent::Type::ToolUseComplete);
    CHECK(events[0].tool_use.tool_use_id == "tu_001");
    CHECK(events[0].tool_use.tool_name == "read_file");
    CHECK(events[0].tool_use.tool_input.value("path", "") == "test.txt");  // partial_json 重组成功
    CHECK(events[1].type == LlmEvent::Type::Done);
    CHECK(ft.captured.url == "https://example.com/v1/messages");
}

static void test_claude_text() {
    FakeTransport ft;
    ft.sse_body =
        "event: content_block_start\n"
        "data: {\"type\":\"content_block_start\",\"index\":0,"
        "\"content_block\":{\"type\":\"text\",\"text\":\"\"}}\n\n"
        "event: content_block_delta\n"
        "data: {\"type\":\"content_block_delta\",\"index\":0,"
        "\"delta\":{\"type\":\"text_delta\",\"text\":\"Hello \"}}\n\n"
        "event: content_block_delta\n"
        "data: {\"type\":\"content_block_delta\",\"index\":0,"
        "\"delta\":{\"type\":\"text_delta\",\"text\":\"world\"}}\n\n"
        "event: content_block_stop\n"
        "data: {\"type\":\"content_block_stop\",\"index\":0}\n\n"
        "event: message_stop\n"
        "data: {\"type\":\"message_stop\"}\n\n";

    ClaudeAdapter adapter(ft.make());
    ProviderConfig cfg;
    cfg.adapter = AdapterType::Claude;
    cfg.base_url = "https://example.com";
    cfg.default_model = "m";

    auto events = collect(adapter, cfg, make_req());
    CHECK(events.size() == 3);
    CHECK(events[0].type == LlmEvent::Type::TextDelta);
    CHECK(events[0].text == "Hello ");
    CHECK(events[1].type == LlmEvent::Type::TextDelta);
    CHECK(events[1].text == "world");
    CHECK(events[2].type == LlmEvent::Type::Done);
}

static void test_claude_translation() {
    FakeTransport ft;
    ft.sse_body = "event: message_stop\ndata: {\"type\":\"message_stop\"}\n\n";

    ClaudeAdapter adapter(ft.make());
    ProviderConfig cfg;
    cfg.adapter = AdapterType::Claude;
    cfg.base_url = "https://example.com";
    cfg.default_model = "m";

    collect(adapter, cfg, make_roundtrip_req());

    const auto body = nlohmann::json::parse(ft.captured.body);
    CHECK(body["model"] == "test-model");
    CHECK(body["tools"][0]["name"] == "read_file");
    CHECK(body["tools"][0]["input_schema"]["required"][0] == "path");

    const auto& msgs = body["messages"];
    CHECK(msgs.size() == 3);
    CHECK(msgs[0]["content"][0]["type"] == "text");
    CHECK(msgs[1]["content"][0]["type"] == "tool_use");
    CHECK(msgs[1]["content"][0]["name"] == "read_file");
    CHECK(msgs[2]["content"][0]["type"] == "tool_result");
    CHECK(msgs[2]["content"][0]["tool_use_id"] == "call_1");
    CHECK(msgs[2]["content"][0]["content"] == "文件内容...");
}

int main() {
    try {
        test_openai_text();
        test_openai_tool();
        test_openai_translation();
        test_claude_tool();
        test_claude_text();
        test_claude_translation();
    } catch (const std::exception& e) {
        std::cerr << "EXCEPTION: " << e.what() << "\n";
        ++g_failures;
    }

    if (g_failures == 0) {
        std::cout << "全部通过\n";
        return 0;
    }
    std::cerr << g_failures << " 处失败\n";
    return 1;
}
