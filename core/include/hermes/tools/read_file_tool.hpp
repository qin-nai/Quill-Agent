#pragma once
#include <fstream>
#include <sstream>
#include <string>
#include "hermes/tool.hpp"
#include "hermes/tools/path_guard.hpp"

namespace hermes {

// P0:读取工作目录内文件。offset + max_lines 分页,防止大文件灌爆 token 预算。
class ReadFileTool : public ITool {
public:
    explicit ReadFileTool(std::filesystem::path workspace_root)
        : root_(std::move(workspace_root)) {}

    ToolSchema schema() const override {
        return {
            "read_file",
            "读取工作目录内指定相对路径的文件内容,用于查看代码、配置文件等文本。"
            "大文件必须用 offset + max_lines 分页,不要一次读整个文件。",
            {
                {"type", "object"},
                {"properties", {
                    {"path", {{"type", "string"}, {"description", "相对于工作目录的文件路径"}}},
                    {"offset", {{"type", "integer"}, {"description", "起始行号(从 1 开始),可选,默认 1"}, {"minimum", 1}}},
                    {"max_lines", {{"type", "integer"}, {"description", "最多读取行数,可选,默认 500"}, {"minimum", 1}, {"maximum", 5000}}}
                }},
                {"required", {"path"}}
            }
        };
    }

    ToolResult execute(const nlohmann::json& input) override {
        const std::string rel = input.at("path").get<std::string>();
        auto full = resolve_within_root(root_, rel);
        if (!full) return {"错误:路径超出工作目录范围", true};

        std::ifstream f(*full, std::ios::binary);
        if (!f) return {"错误:文件不存在或无法读取: " + rel, true};

        const std::size_t offset = input.value("offset", 1ull);
        const std::size_t max_lines = input.value("max_lines", 500ull);

        std::string line;
        std::size_t line_no = 0;
        std::size_t emitted = 0;
        bool truncated = false;
        std::ostringstream out;
        while (std::getline(f, line)) {
            ++line_no;
            if (line_no < offset) continue;
            if (emitted >= max_lines) { truncated = true; break; }
            out << line_no << '\t' << line << '\n';
            ++emitted;
        }
        if (truncated) {
            out << "…(已截断,可用 offset=" << (offset + max_lines) << " 继续读取)\n";
        }
        return {out.str(), false};
    }

private:
    std::filesystem::path root_;
};

} // namespace hermes
