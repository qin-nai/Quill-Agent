#pragma once
#include <regex>
#include <sstream>
#include <string>
#include <vector>
#include "hermes/http_transport.hpp"
#include "hermes/tool.hpp"
#include "hermes/tools/web_utils.hpp"

namespace hermes {

// 网页搜索(无需 API key):抓取 DuckDuckGo HTML 端点解析结果标题+链接+摘要。
class WebSearchTool : public ITool {
public:
    explicit WebSearchTool(HttpTransport transport) : transport_(std::move(transport)) {}

    ToolSchema schema() const override {
        return {
            "web_search",
            "网页搜索,无需 API key。返回结果标题+链接+摘要。适合查资料、找文档、了解最新信息。",
            {
                {"type", "object"},
                {"properties", {
                    {"query", {{"type", "string"}, {"description", "搜索关键词"}}},
                    {"max_results", {{"type", "integer"}, {"description", "最多返回条数,默认 8,上限 15"},
                                     {"minimum", 1}, {"maximum", 15}}},
                }},
                {"required", {"query"}}
            }
        };
    }

    ToolResult execute(const nlohmann::json& input) override {
        const std::string query = input.at("query").get<std::string>();
        const int max_results = static_cast<int>(input.value("max_results", 8));

        HttpRequest hr;
        hr.method = "GET";
        hr.url = "https://html.duckduckgo.com/html/?q=" + url_encode(query);
        hr.headers = {{"user-agent", "Mozilla/5.0 (Hermes Agent)"}};

        std::string body;
        const HttpResponse rsp = transport_(hr, [&](const std::string& chunk) { body += chunk; });
        if (rsp.status == 0)
            return {"错误:网络请求失败" + (rsp.error.empty() ? "" : ": " + rsp.error), true};
        if (rsp.status != 200) {
            if (body.find("anomaly") != std::string::npos)
                return {"错误:搜索被限流(反爬),可改用 web_fetch 访问搜索页面", true};
            return {"错误:HTTP " + std::to_string(rsp.status), true};
        }

        // 解析 result__a 标题+链接 与 result__snippet 摘要,按序配对
        std::vector<std::pair<std::string, std::string>> titles;  // (title, href)
        {
            const std::regex re_a(R"re(<a[^>]*class="result__a"[^>]*href="([^"]*)"[^>]*>(.*?)</a>)re",
                                  std::regex::ECMAScript);
            for (std::sregex_iterator it(body.begin(), body.end(), re_a), end; it != end; ++it) {
                std::string href = (*it)[1].str();
                const std::string title = html_to_text((*it)[2].str());
                const std::string marker = "uddg=";
                const size_t u = href.find(marker);
                if (href.rfind("//duckduckgo.com/l/", 0) == 0 && u != std::string::npos) {
                    std::string enc = href.substr(u + marker.size());
                    const size_t amp = enc.find('&');
                    if (amp != std::string::npos) enc = enc.substr(0, amp);
                    href = url_decode(enc);
                }
                titles.emplace_back(title, href);
            }
        }
        std::vector<std::string> snippets;
        {
            const std::regex re_s(R"re(<a[^>]*class="result__snippet"[^>]*>(.*?)</a>)re",
                                  std::regex::ECMAScript);
            for (std::sregex_iterator it(body.begin(), body.end(), re_s), end; it != end; ++it)
                snippets.push_back(html_to_text((*it)[1].str()));
        }

        if (titles.empty())
            return {html_to_text(body).substr(0, 2000), false};  // fallback:全文前 2000 字

        std::ostringstream os;
        const size_t n = std::min<size_t>(titles.size(), static_cast<size_t>(max_results));
        for (size_t i = 0; i < n; ++i) {
            os << (i + 1) << ". " << titles[i].first << "\n   " << titles[i].second;
            if (i < snippets.size() && !snippets[i].empty())
                os << "\n   " << snippets[i];
            os << "\n";
        }
        return {os.str(), false};
    }

private:
    HttpTransport transport_;
};

} // namespace hermes
