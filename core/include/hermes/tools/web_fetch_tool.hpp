#pragma once
#include <string>
#include "hermes/http_transport.hpp"
#include "hermes/tool.hpp"
#include "hermes/tools/web_utils.hpp"

namespace hermes {

// 抓取 URL 并转可读文本。传输由外部注入(WinHTTP 等)。
class WebFetchTool : public ITool {
public:
    explicit WebFetchTool(HttpTransport transport) : transport_(std::move(transport)) {}

    ToolSchema schema() const override {
        return {
            "web_fetch",
            "抓取指定 URL 的网页内容并转为可读文本(去除 HTML 标签),用于读取网页、文档、在线资料。仅支持 http/https。",
            {
                {"type", "object"},
                {"properties", {
                    {"url", {{"type", "string"},
                             {"description", "要抓取的完整 URL,如 https://example.com/docs"}}},
                    {"max_chars", {{"type", "integer"}, {"description", "最多返回字符数,默认 20000"},
                                   {"minimum", 100}, {"maximum", 100000}}},
                }},
                {"required", {"url"}}
            }
        };
    }

    ToolResult execute(const nlohmann::json& input) override {
        const std::string url = input.at("url").get<std::string>();
        const size_t max_chars = input.value("max_chars", 20000ull);
        if (url.rfind("http://", 0) != 0 && url.rfind("https://", 0) != 0)
            return {"错误:仅支持 http:// 或 https://", true};

        HttpRequest hr;
        hr.method = "GET";
        hr.url = url;
        hr.headers = {{"user-agent", "Mozilla/5.0 (Hermes Agent)"}};

        std::string body;
        body.reserve(65536);
        const HttpResponse rsp = transport_(hr, [&](const std::string& chunk) {
            if (body.size() < max_chars) body += chunk;
        });
        if (rsp.status == 0)
            return {"错误:网络请求失败" + (rsp.error.empty() ? "" : ": " + rsp.error), true};
        if (rsp.status != 200)
            return {"错误:HTTP " + std::to_string(rsp.status), true};
        if (!rsp.content_type.empty()) {
            const std::string ct = rsp.content_type.substr(0, rsp.content_type.find(';'));
            const bool text_ok = ct.rfind("text/", 0) == 0 || ct == "application/json" ||
                                 ct == "application/xhtml+xml" || ct == "application/xml";
            if (!text_ok)
                return {"错误:非文本内容(Content-Type: " + ct + ")", true};
        }
        std::string text = html_to_text(body);
        if (text.size() > max_chars) text = text.substr(0, max_chars) + "\n…(已截断)";
        if (text.empty()) return {"(页面无可见文本)", false};
        return {text, false};
    }

private:
    HttpTransport transport_;
};

} // namespace hermes
