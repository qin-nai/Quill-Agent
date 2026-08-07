#include "app_context.hpp"
#include "mcp_client.hpp"
#include "tool_factory.hpp"
#include <cstdlib>

namespace hermes {

AppContext::AppContext(std::filesystem::path webui, std::filesystem::path ws,
                       std::filesystem::path data)
    : webui_root(std::move(webui)), workspace_root(std::move(ws)), data_dir(std::move(data)),
      sessions(data_dir / "hermes.db"), keys(data_dir / "hermes.db"),
      models(data_dir / "hermes.db") {
    if (const char* home = std::getenv("USERPROFILE"))
        user_skills_root = std::filesystem::path(home) / ".hermes" / "skills";
    register_tools(*this);
}

AppContext::~AppContext() = default;

std::optional<ProviderConfig> resolve_config(const AppContext& ctx, const std::string& provider_id,
                                             const std::string& model) {
    for (const auto& p : builtin_providers()) {
        if (p.id != provider_id) continue;
        ProviderConfig cfg = p;
        if (!model.empty()) cfg.default_model = model;
        // 用户勾选 1M 的模型覆盖默认上下文(256k)
        if (auto w = ctx.models.get_ctx(provider_id, cfg.default_model))
            cfg.context_window = *w;
        auto key = ctx.keys.get(provider_id);
        if (!key) return std::nullopt;  // 无 key → 调用方报 missing key
        cfg.api_key_ref = *key;
        return cfg;
    }
    return std::nullopt;
}

} // namespace hermes
