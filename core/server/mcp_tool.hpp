#pragma once
#include "mcp_client.hpp"
#include "hermes/tool.hpp"

namespace hermes {

// 把远端 MCP 工具包装成本地 ITool。schema 直接透传远端 inputSchema(本就是 JSON Schema)。
class McpRemoteTool : public ITool {
public:
    McpRemoteTool(std::string full_name, McpToolInfo info, McpClient& client)
        : full_name_(std::move(full_name)), info_(std::move(info)), client_(client) {}

    ToolSchema schema() const override {
        return {full_name_, info_.description, info_.input_schema};
    }
    ToolResult execute(const nlohmann::json& input) override {
        return client_.call(info_.name, input);
    }

private:
    std::string full_name_;
    McpToolInfo info_;
    McpClient& client_;
};

} // namespace hermes
