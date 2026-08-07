#include "mcp_client.hpp"
#include "llm/sse_reader.hpp"

namespace hermes {

namespace {
using json = nlohmann::json;

// 从响应提取 JSON:content_type 为 event-stream 或 body 以 event:/data: 开头时,取第一个事件的 data。
json parse_response(const std::string& body, const std::string& content_type, bool& ok) {
    ok = true;
    std::string head = body;
    while (!head.empty() && (head[0] == ' ' || head[0] == '\n' || head[0] == '\r')) head.erase(0, 1);
    const bool sse = content_type.find("text/event-stream") != std::string::npos ||
                     head.rfind("event:", 0) == 0 || head.rfind("data:", 0) == 0;
    if (sse) {
        std::string data;
        detail::SseReader reader([&](const std::string&, const std::string& d) {
            if (data.empty() && !d.empty()) data = d;
        });
        reader.feed(body);
        reader.flush();
        if (data.empty()) { ok = false; return json(); }
        try { return json::parse(data); } catch (...) { ok = false; return json(); }
    }
    try { return json::parse(body); } catch (...) { ok = false; return json(); }
}
} // namespace

McpClient::McpClient(std::string server_name, std::string url, HttpTransport transport)
    : server_name_(std::move(server_name)), url_(std::move(url)),
      transport_(std::move(transport)) {
    bool ok = false;
    const json init = rpc("initialize",
                          {{"protocolVersion", "2025-06-18"}, {"capabilities", json::object()},
                           {"clientInfo", {{"name", "hermes-agent"}, {"version", "0.1"}}}},
                          false, ok);
    if (!ok || !init.contains("result")) return;
    rpc("notifications/initialized", json::object(), true, ok);
    const json tl = rpc("tools/list", json::object(), false, ok);
    if (!ok || !tl.contains("result")) return;
    for (const auto& t : tl["result"].value("tools", json::array())) {
        McpToolInfo info;
        info.name = t.value("name", "");
        info.description = t.value("description", "");
        info.input_schema = t.value("inputSchema", json::object());
        if (!info.name.empty()) tools_.push_back(std::move(info));
    }
    ok_ = true;
}

json McpClient::rpc(const std::string& method, const json& params,
                    bool is_notification, bool& net_ok) {
    net_ok = false;
    json req{{"jsonrpc", "2.0"}, {"method", method}};
    if (!is_notification) req["id"] = ++id_;
    if (!params.is_null()) req["params"] = params;

    HttpRequest hr;
    hr.method = "POST";
    hr.url = url_;
    hr.headers = {{"content-type", "application/json"},
                  {"accept", "application/json, text/event-stream"}};
    if (!session_id_.empty()) hr.headers.emplace_back("mcp-session-id", session_id_);
    hr.body = req.dump();

    std::string body;
    const HttpResponse rsp = transport_(hr, [&](const std::string& chunk) { body += chunk; });
    if (!rsp.mcp_session_id.empty()) session_id_ = rsp.mcp_session_id;
    if (rsp.status != 200) return json();
    return parse_response(body, rsp.content_type, net_ok);
}

ToolResult McpClient::call(const std::string& tool_name, const json& arguments) {
    bool ok = false;
    const json j = rpc("tools/call", {{"name", tool_name}, {"arguments", arguments}}, false, ok);
    if (!ok) return {"错误:MCP 调用失败(网络或响应解析)", true};
    if (j.contains("error"))
        return {"错误:MCP " + j["error"].value("message", "未知错误"), true};
    if (!j.contains("result")) return {"错误:MCP 无 result", true};

    const json& result = j["result"];
    const bool is_error = result.value("isError", false);
    std::string text;
    for (const auto& c : result.value("content", json::array())) {
        const std::string type = c.value("type", "");
        if (type == "text") {
            text += c.value("text", "");
        } else if (type == "image") {
            text += "(image 已忽略)\n";
        } else {
            text += c.value("text", "");
        }
    }
    if (text.empty()) text = "(无内容)";
    return {text, is_error};
}

} // namespace hermes
