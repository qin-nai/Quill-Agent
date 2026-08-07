#pragma once
#include <cctype>
#include <filesystem>
#include <fstream>
#include <regex>
#include <sstream>
#include <string>
#include "hermes/tool.hpp"
#include "hermes/tools/glob.hpp"
#include "hermes/tools/path_guard.hpp"

namespace hermes {

// 在目录内按正则搜索文件内容,返回 相对路径:行号: 文本。
// ECMAScript 正则(无 lookaround/反向引用);编译失败自动退化为纯文本子串匹配。
class GrepTool : public ITool {
public:
    explicit GrepTool(std::filesystem::path root) : root_(std::move(root)) {}

    ToolSchema schema() const override {
        return {
            "grep",
            "在工作目录内按正则搜索文件内容,返回 相对路径:行号: 文本。ECMAScript 正则,不支持 lookaround/反向引用;"
            "编译失败自动退化为纯文本子串匹配。",
            {
                {"type", "object"},
                {"properties", {
                    {"pattern", {{"type", "string"}, {"description", "要搜索的正则或子串"}}},
                    {"path", {{"type", "string"}, {"description", "搜索起始相对路径,默认 . 表示整个工作目录"}}},
                    {"case_sensitive", {{"type", "boolean"}, {"description", "默认 false(忽略大小写)"}}},
                    {"glob", {{"type", "string"}, {"description", "可选:只搜匹配该 glob 的文件"}}},
                    {"max_lines", {{"type", "integer"}, {"description", "最多返回行数,默认 100"},
                                   {"minimum", 1}, {"maximum", 1000}}},
                }},
                {"required", {"pattern"}}
            }
        };
    }

    ToolResult execute(const nlohmann::json& input) override {
        const std::string pattern = input.at("pattern").get<std::string>();
        const std::string base_rel = input.value("path", std::string("."));
        auto base = resolve_within_root(root_, base_rel);
        if (!base) return {"错误:路径超出工作目录", true};
        const bool ci = !input.value("case_sensitive", false);
        const std::string glob_filter = input.value("glob", std::string());
        const int max_lines = static_cast<int>(input.value("max_lines", 100));

        std::regex re;
        bool use_regex = true;
        try {
            auto flags = std::regex_constants::ECMAScript;
            if (ci) flags |= std::regex_constants::icase;
            re = std::regex(pattern, flags);
        } catch (const std::regex_error&) {
            use_regex = false;
        }

        std::ostringstream os;
        int found = 0;
        std::error_code ec;
        for (auto it = std::filesystem::recursive_directory_iterator(
                 *base, std::filesystem::directory_options::skip_permission_denied, ec);
             !ec && it != std::filesystem::recursive_directory_iterator{}; it.increment(ec)) {
            if (it->is_directory(ec) && skip_dir(it->path().filename().string())) {
                it.disable_recursion_pending();
                continue;
            }
            if (!it->is_regular_file(ec)) continue;
            const std::string rel = it->path().lexically_relative(root_).generic_string();
            if (!glob_filter.empty() && !glob_match(glob_filter, rel)) continue;

            std::ifstream f(it->path(), std::ios::binary);
            if (!f) continue;
            if (is_binary(f)) continue;  // 探测 \0 后重置流

            std::string line;
            size_t lineno = 0;
            while (std::getline(f, line)) {
                ++lineno;
                if (line.find('\0') != std::string::npos) break;
                const bool hit = use_regex ? std::regex_search(line, re)
                                           : (ci ? contains_ci(line, pattern)
                                                 : line.find(pattern) != std::string::npos);
                if (hit) {
                    os << rel << ":" << lineno << ": " << line << "\n";
                    if (++found >= max_lines) break;
                }
            }
            if (found >= max_lines) break;
        }
        if (found == 0) return {"无匹配", false};
        if (found >= max_lines) os << "…(已截断,可用更精确的 pattern)";
        return {os.str(), false};
    }

private:
    static bool skip_dir(const std::string& n) {
        return n == "data" || n == ".git" || n == "build" || n == "node_modules";
    }
    static bool contains_ci(const std::string& hay, const std::string& needle) {
        if (needle.empty()) return true;
        for (size_t i = 0; i + needle.size() <= hay.size(); ++i) {
            size_t j = 0;
            while (j < needle.size() &&
                   std::tolower(static_cast<unsigned char>(hay[i + j])) ==
                       std::tolower(static_cast<unsigned char>(needle[j])))
                ++j;
            if (j == needle.size()) return true;
        }
        return false;
    }
    // 读前 8KB 探测 \0(二进制);探测后重置流到起点。
    static bool is_binary(std::ifstream& f) {
        char probe[8192];
        f.read(probe, sizeof(probe));
        const std::streamsize got = f.gcount();
        for (std::streamsize k = 0; k < got; ++k)
            if (probe[k] == '\0') return true;
        f.clear();
        f.seekg(0);
        return false;
    }
    std::filesystem::path root_;
};

} // namespace hermes
