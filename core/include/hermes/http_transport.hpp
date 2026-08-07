#pragma once
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace hermes {

struct HttpRequest {
    std::string method;  // "POST"
    std::string url;
    std::vector<std::pair<std::string, std::string>> headers;
    std::string body;
};

struct HttpResponse {
    int status = 0;       // 0 表示网络层失败
    std::string error;    // 网络失败原因
    std::string content_type;     // 响应 Content-Type(如 "text/html"),缺省为空
    std::string mcp_session_id;   // Mcp-Session-Id 响应头(MCP 会话),缺省为空
    bool stream_complete = true;  // body 是否完整读完(超时/异常中断为 false)
};

// 发起 HTTP 请求,按到达顺序回调 body 片段(SSE 场景逐块回调,保持流式)。
// 同步阻塞直到响应读完。M2 用 cpp-httplib + TLS 实现,移动端换平台 TLS。
using BodyChunkCallback = std::function<void(const std::string&)>;
using HttpTransport = std::function<HttpResponse(const HttpRequest&, const BodyChunkCallback&)>;

} // namespace hermes
