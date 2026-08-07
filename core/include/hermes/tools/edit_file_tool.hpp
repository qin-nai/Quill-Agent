#pragma once
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include "hermes/tool.hpp"
#include "hermes/tools/path_guard.hpp"

namespace hermes {

// P0:精准局部替换。old_string 必须在文件中唯一匹配;匹配不到/匹配多处都返回错误,
// 让模型重新 read_file 拿到最新内容。这是代码 agent 的主要产出手段。
class EditFileTool : public ITool {
public:
    explicit EditFileTool(std::filesystem::path workspace_root)
        : root_(std::move(workspace_root)) {}

    ToolSchema schema() const override {
        return {
            "edit_file",
            "在文件中做精确字符串替换,是修改已有代码的首选工具。old_string 必须与文件中一段内容"
            "逐字节一致且唯一;不确定上下文时先 read_file 再编辑。",
            {
                {"type", "object"},
                {"properties", {
                    {"path", {{"type", "string"}, {"description", "相对于工作目录的文件路径"}}},
                    {"old_string", {{"type", "string"}, {"description", "需唯一匹配的原文,不得为空"}}},
                    {"new_string", {{"type", "string"}, {"description", "替换后的内容,可为空串"}}}
                }},
                {"required", {"path", "old_string", "new_string"}}
            }
        };
    }

    bool requires_confirmation() const override { return true; }

    ToolResult execute(const nlohmann::json& input) override {
        const std::string rel = input.at("path").get<std::string>();
        const std::string old_s = input.at("old_string").get<std::string>();
        const std::string new_s = input.at("new_string").get<std::string>();
        if (old_s.empty()) return {"错误:old_string 不能为空", true};

        auto full = resolve_within_root(root_, rel);
        if (!full) return {"错误:路径超出工作目录范围", true};

        std::ifstream in(*full, std::ios::binary);
        if (!in) return {"错误:文件不存在或无法读取: " + rel, true};
        std::ostringstream ss;
        ss << in.rdbuf();
        std::string content = ss.str();
        in.close();

        const std::size_t first = content.find(old_s);
        if (first == std::string::npos)
            return {"错误:old_string 在文件中未找到,请重新 read_file 获取最新内容", true};
        if (content.find(old_s, first + 1) != std::string::npos)
            return {"错误:old_string 匹配到多处,请包含更多上下文使匹配唯一", true};

        content.replace(first, old_s.size(), new_s);

        std::ofstream out(*full, std::ios::binary | std::ios::trunc);
        if (!out) return {"错误:无法写入文件: " + rel, true};
        out << content;
        return {"已替换 " + rel + " 中 1 处匹配", false};
    }

private:
    std::filesystem::path root_;
};

} // namespace hermes
