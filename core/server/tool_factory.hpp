#pragma once
#include "app_context.hpp"

namespace hermes {

// 装配所有内置工具(以及 MCP 客户端)并注册到 ctx.tools。工具实例化逻辑全部收在这里。
void register_tools(AppContext& ctx);

} // namespace hermes
