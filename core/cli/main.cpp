#include <iostream>
#include <memory>
#include <string>
#include <vector>
#include "hermes/agent_loop.hpp"
#include "hermes/llm/llm_client.hpp"
#include "hermes/provider.hpp"
#include "hermes/tool_registry.hpp"
#include "hermes/tools/edit_file_tool.hpp"
#include "hermes/tools/read_file_tool.hpp"
#include "hermes/tools/write_file_tool.hpp"

int main() {
    using namespace hermes;

    std::cout << "== 内置 provider 预设 ==\n";
    for (const auto& p : builtin_providers()) {
        std::cout << "  " << p.id << " (" << p.name << ")"
                  << " adapter=" << (p.adapter == AdapterType::Claude ? "claude" : "openai_compat")
                  << " base=" << (p.base_url.empty() ? "(自定义)" : p.base_url) << "\n";
    }

    ToolRegistry registry;
    registry.register_tool(std::make_unique<ReadFileTool>("./workspace"));
    registry.register_tool(std::make_unique<WriteFileTool>("./workspace"));
    registry.register_tool(std::make_unique<EditFileTool>("./workspace"));

    // M1:用 Stub 验证 loop。adapter=Stub 时 LLMClient 不走网络。
    ProviderConfig stub_cfg;
    stub_cfg.id = "stub";
    stub_cfg.name = "Stub(M1 调试)";
    stub_cfg.adapter = AdapterType::Stub;

    HttpTransport noop_transport = [](const HttpRequest&, const BodyChunkCallback&) {
        HttpResponse r;
        r.status = 0;
        r.error = "stub 不发起网络请求";
        return r;
    };
    LLMClient client(stub_cfg, noop_transport);
    AgentLoop loop(registry);

    std::vector<Message> history;
    history.push_back({"user", {ContentBlock::make_text("帮我看看 test.txt")}});

    const bool ok = loop.run(stub_cfg, client.adapter(), history,
                             [](const ContentBlock& call) {
                                 std::cout << "[确认请求] tool=" << call.tool_name
                                           << " -> 自动放行(M1 测试)\n";
                                 return true;
                             });

    std::cout << "\n== 会话历史 ==\n";
    for (const auto& msg : history) {
        std::cout << "[" << msg.role << "]";
        for (const auto& b : msg.content) {
            switch (b.type) {
                case BlockType::Text:
                    std::cout << " text:" << b.text;
                    break;
                case BlockType::ToolUse:
                    std::cout << " tool_use:" << b.tool_name;
                    break;
                case BlockType::ToolResult:
                    std::cout << " tool_result(" << (b.is_error ? "err" : "ok")
                              << "):" << b.tool_output;
                    break;
                default:
                    break;
            }
        }
        std::cout << "\n";
    }
    return ok ? 0 : 1;
}
