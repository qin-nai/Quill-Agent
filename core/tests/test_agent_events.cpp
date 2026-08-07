#include "hermes/agent_loop.hpp"
#include "hermes/json_io.hpp"
#include "hermes/provider.hpp"
#include "hermes/tool_registry.hpp"
#include "hermes/tools/edit_file_tool.hpp"
#include "hermes/tools/read_file_tool.hpp"
#include <nlohmann/json.hpp>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <string>
#include <vector>

using namespace hermes;

namespace {
int g_fail = 0;
#define CHECK(cond) do { if (!(cond)) { ++g_fail; std::cerr << "FAIL " << __LINE__ << ": " #cond "\n"; } } while (0)

// 脚本适配器:第一次发 tool 调用,收到 tool_result 后再给文本回答。
struct FakeAdapter : IProviderAdapter {
    std::string tool_name;
    void stream(const ProviderConfig&, const LlmRequest& req, const EventCallback& on_event) override {
        bool already_called = false;
        for (const auto& m : req.messages)
            for (const auto& b : m.content)
                if (b.type == BlockType::ToolResult) already_called = true;
        if (!already_called) {
            on_event(LlmEvent{LlmEvent::Type::ToolUseComplete, {},
                              ContentBlock::make_tool_use("tu_x", tool_name, {{"path", "test.txt"}}), ""});
        } else {
            on_event(LlmEvent{LlmEvent::Type::TextDelta, "完成。", {}, ""});
        }
        on_event(LlmEvent{LlmEvent::Type::Done, {}, {}, ""});
    }
};

struct Collector {
    std::vector<AgentEvent> events;
    void operator()(const AgentEvent& e) { events.push_back(e); }
    int count(AgentEvent::Type t) const {
        int n = 0;
        for (const auto& e : events) if (e.type == t) ++n;
        return n;
    }
};

} // namespace

static void test_json_roundtrip() {
    Message m;
    m.role = "user";
    m.content = {ContentBlock::make_text("hello"),
                 ContentBlock::make_tool_use("t1", "read_file", {{"path", "a.txt"}}),
                 ContentBlock::make_tool_result("t1", "ok", false)};
    nlohmann::json j = m;
    const Message m2 = j.get<Message>();
    CHECK(m2.role == "user");
    CHECK(m2.content.size() == 3);
    CHECK(m2.content[0].type == BlockType::Text && m2.content[0].text == "hello");
    CHECK(m2.content[1].type == BlockType::ToolUse && m2.content[1].tool_name == "read_file");
    CHECK(m2.content[1].tool_input.value("path", "") == "a.txt");
    CHECK(m2.content[2].type == BlockType::ToolResult && !m2.content[2].is_error);
}

int main() {
    test_json_roundtrip();

    namespace fs = std::filesystem;
    namespace fs = std::filesystem;
    const fs::path dir = fs::temp_directory_path() / "hermes_agent_events_test";
    fs::remove_all(dir);
    fs::create_directories(dir);
    { std::ofstream f(dir / "test.txt"); f << "hello\n"; }

    ProviderConfig cfg;
    cfg.id = "test";
    cfg.adapter = AdapterType::Stub;

    // test1:read_file 无需确认 → tool_use → tool_result(ok) → done,无 confirm_request
    {
        ToolRegistry reg;
        reg.register_tool(std::make_unique<ReadFileTool>(dir));
        FakeAdapter ad;
        ad.tool_name = "read_file";
        std::vector<Message> history{{"user", {ContentBlock::make_text("hi")}}};
        Collector c;
        AgentLoop loop(reg);
        loop.run(cfg, ad, history, [](const ContentBlock&) { return true; }, std::ref(c));
        CHECK(c.count(AgentEvent::Type::ToolUse) == 1);
        CHECK(c.count(AgentEvent::Type::ConfirmRequest) == 0);
        CHECK(c.count(AgentEvent::Type::ToolResult) == 1);
        CHECK(c.count(AgentEvent::Type::Done) == 1);
        for (const auto& e : c.events)
            if (e.type == AgentEvent::Type::ToolResult) CHECK(e.tool_ok);
    }

    // test2:edit_file 需确认且被拒绝 → confirm_request + tool_result(err,含"拒绝")
    {
        ToolRegistry reg;
        reg.register_tool(std::make_unique<EditFileTool>(dir));
        FakeAdapter ad;
        ad.tool_name = "edit_file";
        std::vector<Message> history{{"user", {ContentBlock::make_text("edit")}}};
        Collector c;
        AgentLoop loop(reg);
        loop.run(cfg, ad, history, [](const ContentBlock&) { return false; }, std::ref(c));
        CHECK(c.count(AgentEvent::Type::ConfirmRequest) == 1);
        CHECK(c.count(AgentEvent::Type::ToolResult) == 1);
        CHECK(c.count(AgentEvent::Type::Done) == 1);
        for (const auto& e : c.events)
            if (e.type == AgentEvent::Type::ToolResult) {
                CHECK(!e.tool_ok);
                CHECK(e.tool_output.find("拒绝") != std::string::npos);
            }
    }

    fs::remove_all(dir);

    if (g_fail) {
        std::cerr << g_fail << " 项失败\n";
        return 1;
    }
    std::cout << "agent events 测试通过\n";
    return 0;
}
