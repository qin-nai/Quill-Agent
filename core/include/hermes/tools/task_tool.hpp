#pragma once
#include "hermes/task_runner.hpp"
#include "hermes/tool.hpp"

namespace hermes {

// 子代理工具:把独立小任务交给子代理完成(内层 AgentLoop,同工具集),返回其最终文本。
class TaskTool : public ITool {
public:
    ToolSchema schema() const override {
        return {
            "task",
            "启动一个子任务(sub-agent):把独立的小任务交给子代理完成,返回其最终回答文本。"
            "子代理拥有相同的工具集,可嵌套最多 3 层。适合并行调研、独立子步骤。",
            {
                {"type", "object"},
                {"properties", {
                    {"prompt", {{"type", "string"}, {"description", "给子代理的完整任务指令"}}},
                }},
                {"required", {"prompt"}}
            }
        };
    }
    ToolResult execute(const nlohmann::json& input) override {
        auto runner = get_task_runner();
        if (!runner) return {"错误:task 工具只能在 agent 会话中调用", true};
        return runner(input.at("prompt").get<std::string>());
    }
};

} // namespace hermes
