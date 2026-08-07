#include "hermes/tools/run_script_tool.hpp"
#include "run_script_bridge.h"
#include <cstdlib>

namespace hermes {

RunScriptTool::RunScriptTool(std::filesystem::path workspace_root)
    : root_(std::move(workspace_root)) {}

ToolSchema RunScriptTool::schema() const {
    return {
        "run_script",
        "在受限的 JavaScript 沙盒中执行一段脚本(QuickJS 解释器),用于计算、文本/数据处理、"
        "文件内容分析。脚本可直接写 JS:算术、字符串/数组/对象、JSON 处理、循环等。"
        "沙盒只读,可用的全局函数:readFile(路径)——读取工作目录内文件文本;"
        "listDir(路径)——列出目录条目;glob(模式)——按 glob 匹配工作目录内文件相对路径;"
        "console.log/error 输出会进入结果。不能访问 shell、网络、进程、写文件——需要写文件请用 write_file。"
        "脚本超过 5 秒会被强制中断。",
        {
            {"type", "object"},
            {"properties", {
                {"script", {{"type", "string"}, {"description", "要执行的 JavaScript 代码"}}}
            }},
            {"required", {"script"}}
        }
    };
}

ToolResult RunScriptTool::execute(const nlohmann::json& input) {
    const std::string script = input.value("script", "");
    if (script.empty()) return {"错误:缺少 script 参数", true};

    char* out = nullptr;
    int is_error = 0;
    hermes_script_run(root_.string().c_str(), script.c_str(), &out, &is_error);
    std::string result = out ? out : "(无输出)";
    if (out) std::free(out);
    return {result, is_error != 0};
}

} // namespace hermes
