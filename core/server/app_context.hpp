#pragma once
#include <atomic>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>
#include "hermes/provider.hpp"
#include "hermes/tool_registry.hpp"
#include "keystore.hpp"
#include "model_store.hpp"
#include "run_context.hpp"
#include "session_store.hpp"

namespace hermes {

class McpClient;  // 前置声明;完整定义在 mcp_client.hpp

// server 全量共享状态。构造即打开 sqlite、装配工具(经 tool_factory)。
struct AppContext {
    std::filesystem::path webui_root, workspace_root, data_dir;
    std::filesystem::path user_skills_root;      // <USERPROFILE>/.hermes/skills,构造时计算
    std::atomic<bool> fetch_abort{false};        // 网页工具传输的取消信号(当前恒 false,预留)
    // 顺序关键:McpRemoteTool 持有 McpClient 引用,故 mcp_clients 必须在 tools 之前声明
    //(析构时按逆序,先析构 tools 再析构 mcp_clients,不留悬垂引用)。
    std::vector<std::unique_ptr<McpClient>> mcp_clients;
    ToolRegistry tools;
    SessionStore sessions;
    KeyStore keys;
    ModelStore models;   // 用户自定义模型(内置在 provider.cpp,展示时合并)

    // 运行中回合:session_id → RunContext(用于 confirm/stop 路由)。
    std::mutex active_mtx;
    std::unordered_map<std::string, RunContextPtr> active;

    AppContext(std::filesystem::path webui, std::filesystem::path ws,
               std::filesystem::path data);
    ~AppContext();  // 在 cpp 定义(mcp_clients 需 McpClient 完整类型)
};

// 复制内置预设,覆盖 default_model 为会话所选模型;该 provider 无 key 返回 nullopt(调用方报 missing key)。
std::optional<ProviderConfig> resolve_config(const AppContext& ctx, const std::string& provider_id,
                                             const std::string& model);

} // namespace hermes
