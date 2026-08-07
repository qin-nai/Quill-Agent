#pragma once
#include <string>
#include <vector>
#include <nlohmann/json.hpp>
#include "hermes/http_transport.hpp"
#include "hermes/tool.hpp"

namespace hermes {

struct McpToolInfo {
    std::string name;
    std::string description;
    nlohmann::json input_schema;
};

// MCP streamable-HTTP 客户端:JSON-RPC 2.0 over HTTP。握手 initialize → notifications/initialized → tools/list。
// 独立模块;传输由外部注入(复用 WinHTTP HttpTransport)。不处理鉴权(OAuth/Bearer 暂不支持)。
class McpClient {
public:
    McpClient(std::string server_name, std::string url, HttpTransport transport);

    bool ok() const { return ok_; }
    const std::string& server_name() const { return server_name_; }
    const std::vector<McpToolInfo>& tools() const { return tools_; }

    // 调用远端工具 tools/call,返回文本拼接结果。
    ToolResult call(const std::string& tool_name, const nlohmann::json& arguments);

private:
    nlohmann::json rpc(const std::string& method, const nlohmann::json& params,
                       bool is_notification, bool& net_ok);

    std::string server_name_, url_;
    HttpTransport transport_;
    std::string session_id_;  // Mcp-Session-Id 回传
    int id_ = 0;
    bool ok_ = false;
    std::vector<McpToolInfo> tools_;
};

} // namespace hermes
