#include "mcp_client.hpp"
#include <nlohmann/json.hpp>
#include <iostream>
#include <string>

using namespace hermes;

static int g_fail = 0;
#define CHECK(cond) do { if (!(cond)) { ++g_fail; std::cerr << "FAIL " << __LINE__ << ": " #cond "\n"; } } while (0)

// 假传输:按 JSON-RPC method 回放响应,记录 session_id 传递。
static HttpTransport make_fake(bool sse_mode) {
    return [sse_mode](const HttpRequest& hr, const BodyChunkCallback& cb) -> HttpResponse {
        HttpResponse out;
        out.status = 200;
        out.content_type = "application/json";
        nlohmann::json req;
        try { req = nlohmann::json::parse(hr.body); } catch (...) { out.status = 500; return out; }
        const std::string method = req.value("method", "");

        const auto respond = [&](const std::string& json_body) {
            if (sse_mode) {
                out.content_type = "text/event-stream";
                cb("event: message\ndata: " + json_body + "\n\n");
            } else {
                cb(json_body);
            }
        };

        if (method == "initialize") {
            out.mcp_session_id = "sess-abc";
            respond("{\"jsonrpc\":\"2.0\",\"id\":1,\"result\":{\"protocolVersion\":\"2025-06-18\","
                    "\"capabilities\":{},\"serverInfo\":{}}}");
        } else if (method == "tools/list") {
            // 校验 session_id 是否回传
            bool has_session = false;
            for (const auto& h : hr.headers)
                if (h.first == "mcp-session-id" && h.second == "sess-abc") has_session = true;
            CHECK(has_session);
            respond("{\"jsonrpc\":\"2.0\",\"id\":2,\"result\":{\"tools\":["
                    "{\"name\":\"read_file\",\"description\":\"读文件\","
                    "\"inputSchema\":{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"}}}}]}}");
        } else if (method == "tools/call") {
            CHECK(req["params"].value("name", "") == "read_file");
            CHECK(req["params"]["arguments"].value("path", "") == "a.txt");
            respond("{\"jsonrpc\":\"2.0\",\"id\":3,\"result\":{\"content\":"
                    "[{\"type\":\"text\",\"text\":\"hello from mcp\"}],\"isError\":false}}");
        } else {
            respond("{}");  // notifications/initialized 等
        }
        return out;
    };
}

int main() {
    std::atomic<bool> abort{false};

    // 用例1:JSON 响应
    {
        McpClient c("fs", "http://127.0.0.1:9999/mcp", make_fake(false));
        CHECK(c.ok());
        CHECK(c.server_name() == "fs");
        CHECK(c.tools().size() == 1);
        CHECK(c.tools()[0].name == "read_file");
        CHECK(c.tools()[0].input_schema.value("type", "") == "object");
        const ToolResult r = c.call("read_file", {{"path", "a.txt"}});
        CHECK(!r.is_error && r.output == "hello from mcp");
    }

    // 用例2:SSE 响应
    {
        McpClient c("fs2", "http://127.0.0.1:9999/mcp", make_fake(true));
        CHECK(c.ok());
        const ToolResult r = c.call("read_file", {{"path", "a.txt"}});
        CHECK(!r.is_error && r.output == "hello from mcp");
    }

    if (g_fail) {
        std::cerr << g_fail << " 项失败\n";
        return 1;
    }
    std::cout << "mcp 测试通过\n";
    return 0;
}
