#include "tool_factory.hpp"
#include "hermes/tools/edit_file_tool.hpp"
#include "hermes/tools/glob_tool.hpp"
#include "hermes/tools/grep_tool.hpp"
#include "hermes/tools/read_file_tool.hpp"
#include "hermes/tools/read_skill_tool.hpp"
#include "hermes/tools/run_script_tool.hpp"
#include "hermes/tools/task_tool.hpp"
#include "hermes/tools/todo_tool.hpp"
#include "hermes/tools/web_fetch_tool.hpp"
#include "hermes/tools/web_search_tool.hpp"
#include "hermes/tools/write_file_tool.hpp"
#include "http_transport_winhttp.hpp"
#include "mcp_client.hpp"
#include "mcp_tool.hpp"
#include <fstream>

namespace hermes {

namespace {

using json = nlohmann::json;

// 读 data_dir/mcp_servers.json,对每个 server 握手并注册其工具为 mcp_<server>_<tool>。
// 缺文件/解析失败/握手失败 → 跳过,不报错。
void register_mcp(AppContext& ctx, const std::filesystem::path& cfg_path,
                  const HttpTransport& transport) {
    std::ifstream f(cfg_path);
    if (!f) return;
    json cfg;
    try { cfg = json::parse(f); } catch (...) { return; }
    for (const auto& s : cfg.value("servers", json::array())) {
        const std::string name = s.value("name", "");
        const std::string url = s.value("url", "");
        if (name.empty() || url.empty()) continue;
        auto client = std::make_unique<McpClient>(name, url, transport);
        if (!client->ok()) continue;  // 握手失败,跳过该 server
        for (const auto& t : client->tools()) {
            ctx.tools.register_tool(std::make_unique<McpRemoteTool>(
                "mcp_" + name + "_" + t.name, t, *client));
        }
        ctx.mcp_clients.push_back(std::move(client));
    }
}

} // namespace

void register_tools(AppContext& ctx) {
    // 网页工具共用 15s 超时传输;abort 信号引用 AppContext::fetch_abort
    const auto web_transport = make_http_transport(ctx.fetch_abort, 15000);
    // MCP 握手 8s 超时,启动时阻塞执行
    const auto mcp_transport = make_http_transport(ctx.fetch_abort, 8000);

    ctx.tools.register_tool(std::make_unique<ReadFileTool>(ctx.workspace_root));
    ctx.tools.register_tool(std::make_unique<WriteFileTool>(ctx.workspace_root));
    ctx.tools.register_tool(std::make_unique<EditFileTool>(ctx.workspace_root));
    ctx.tools.register_tool(std::make_unique<WebFetchTool>(web_transport));
    ctx.tools.register_tool(std::make_unique<WebSearchTool>(web_transport));
    ctx.tools.register_tool(std::make_unique<GlobTool>(ctx.workspace_root));
    ctx.tools.register_tool(std::make_unique<GrepTool>(ctx.workspace_root));
    ctx.tools.register_tool(std::make_unique<TodoTool>());
    ctx.tools.register_tool(std::make_unique<ReadSkillTool>(ctx.workspace_root, ctx.user_skills_root));
    ctx.tools.register_tool(std::make_unique<TaskTool>());
    ctx.tools.register_tool(std::make_unique<RunScriptTool>(ctx.workspace_root));

    register_mcp(ctx, ctx.data_dir / "mcp_servers.json", mcp_transport);
}

} // namespace hermes
