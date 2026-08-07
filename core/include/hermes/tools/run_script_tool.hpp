#pragma once
#include <filesystem>
#include "hermes/tool.hpp"

namespace hermes {

// 受限 JS 沙盒(QuickJS):让 agent 真写并执行 JS(算法/文本/数据处理)。
// MVP 只读:注入 readFile/listDir/glob(全部限定在工作目录内)+ console.log/error。
// 护栏:内存 64MB + 5s 超时中断。不暴露写/网络/模块/进程。
// 写文件仍走 write_file/edit_file(带人工确认)。
class RunScriptTool : public ITool {
public:
    explicit RunScriptTool(std::filesystem::path workspace_root);

    ToolSchema schema() const override;
    ToolResult execute(const nlohmann::json& input) override;
    bool requires_confirmation() const override { return false; }

private:
    std::filesystem::path root_;
};

} // namespace hermes
