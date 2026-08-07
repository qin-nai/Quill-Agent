#pragma once
#include <filesystem>
#include <sstream>
#include <vector>
#include "hermes/tool.hpp"
#include "hermes/tools/glob.hpp"

namespace hermes {

// 按 glob 模式递归匹配工作目录内文件,返回相对路径列表。
class GlobTool : public ITool {
public:
    explicit GlobTool(std::filesystem::path root) : root_(std::move(root)) {}

    ToolSchema schema() const override {
        return {
            "glob",
            "按 glob 模式递归匹配工作目录内文件,返回相对路径列表。* 不跨目录,** 跨目录,? 单字符,[abc]/[!abc] 字符类。",
            {
                {"type", "object"},
                {"properties", {
                    {"pattern", {{"type", "string"},
                                 {"description", "glob 模式,如 **/*.cpp 或 src/*.hpp"}}},
                    {"max_results", {{"type", "integer"}, {"description", "最多返回条数,默认 100,上限 1000"},
                                     {"minimum", 1}, {"maximum", 1000}}},
                }},
                {"required", {"pattern"}}
            }
        };
    }

    ToolResult execute(const nlohmann::json& input) override {
        const std::string pattern = input.at("pattern").get<std::string>();
        const int max_results = static_cast<int>(input.value("max_results", 100));
        std::vector<std::string> hits;
        std::error_code ec;
        for (auto it = std::filesystem::recursive_directory_iterator(
                 root_, std::filesystem::directory_options::skip_permission_denied, ec);
             !ec && it != std::filesystem::recursive_directory_iterator{}; it.increment(ec)) {
            if (it->is_directory(ec) && skip_dir(it->path().filename().string())) {
                it.disable_recursion_pending();
                continue;
            }
            if (!it->is_regular_file(ec)) continue;
            const std::string rel = it->path().lexically_relative(root_).generic_string();
            if (glob_match(pattern, rel)) hits.push_back(rel);
            if (static_cast<int>(hits.size()) >= max_results) break;
        }
        if (hits.empty()) return {"无匹配文件", false};
        std::ostringstream os;
        for (const auto& h : hits) os << h << "\n";
        return {os.str(), false};
    }

private:
    static bool skip_dir(const std::string& n) {
        return n == "data" || n == ".git" || n == "build" || n == "node_modules";
    }
    std::filesystem::path root_;
};

} // namespace hermes
