#pragma once
#include <filesystem>
#include <fstream>
#include <string>
#include "hermes/tool.hpp"
#include "hermes/tools/path_guard.hpp"

namespace hermes {

// P1:整文件覆盖写入。仅用于新建小文件或整体重写;改大文件局部用 edit_file。
class WriteFileTool : public ITool {
public:
    explicit WriteFileTool(std::filesystem::path workspace_root)
        : root_(std::move(workspace_root)) {}

    ToolSchema schema() const override {
        return {
            "write_file",
            "向工作目录内指定相对路径写入文件内容,会覆盖已有内容。仅用于新建小文件或整体重写;"
            "修改已有大文件时改用 edit_file 做局部替换。",
            {
                {"type", "object"},
                {"properties", {
                    {"path", {{"type", "string"}, {"description", "相对于工作目录的文件路径"}}},
                    {"content", {{"type", "string"}}}
                }},
                {"required", {"path", "content"}}
            }
        };
    }

    bool requires_confirmation() const override { return true; }

    ToolResult execute(const nlohmann::json& input) override {
        const std::string rel = input.at("path").get<std::string>();
        const std::string content = input.at("content").get<std::string>();
        auto full = resolve_within_root(root_, rel);
        if (!full) return {"错误:路径超出工作目录范围", true};

        std::error_code ec;
        std::filesystem::create_directories((*full).parent_path(), ec);
        if (ec) return {"错误:无法创建父目录: " + rel, true};

        std::ofstream f(*full, std::ios::binary | std::ios::trunc);
        if (!f) return {"错误:无法写入文件: " + rel, true};
        f << content;
        return {"已写入 " + rel + "(" + std::to_string(content.size()) + " bytes)", false};
    }

private:
    std::filesystem::path root_;
};

} // namespace hermes
