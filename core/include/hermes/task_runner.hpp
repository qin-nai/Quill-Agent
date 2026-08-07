#pragma once
#include <functional>
#include "hermes/tool.hpp"

namespace hermes {

// 子代理执行器(thread_local):AgentLoop::run 安装当前回合闭包,TaskTool 调用它执行内层 run。
// 用 thread_local 而非注册表字段,避免并发会话共享工具单例时发生竞争。
using TaskRunner = std::function<ToolResult(const std::string& prompt)>;
void set_task_runner(TaskRunner r);
TaskRunner get_task_runner();

} // namespace hermes
