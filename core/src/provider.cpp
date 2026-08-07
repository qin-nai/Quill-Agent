#include "hermes/provider.hpp"

namespace hermes {

namespace {
ProviderConfig anth(std::string id, std::string name, std::string base,
                    std::vector<std::string> models, std::string def, size_t ctx) {
    ProviderConfig c;
    c.id = std::move(id);
    c.name = std::move(name);
    c.adapter = AdapterType::Claude;
    c.base_url = std::move(base);
    c.models = std::move(models);
    c.default_model = std::move(def);
    c.context_window = ctx;
    return c;
}

ProviderConfig oai(std::string id, std::string name, std::string base,
                   std::vector<std::string> models, std::string def, size_t ctx) {
    ProviderConfig c;
    c.id = std::move(id);
    c.name = std::move(name);
    c.adapter = AdapterType::OpenAICompat;
    c.base_url = std::move(base);
    c.models = std::move(models);
    c.default_model = std::move(def);
    c.context_window = ctx;
    return c;
}
} // namespace

std::vector<ProviderConfig> builtin_providers() {
    return {
        // ── Anthropic 兼容:原生 Messages 协议,改 base_url 即用 ──
        anth("anthropic", "Claude 官方", "https://api.anthropic.com",
             {"claude-opus-4-8", "claude-sonnet-4-6", "claude-haiku-4-5"},
             "claude-opus-4-8", 256000),
        []() {
            ProviderConfig c = anth("deepseek", "DeepSeek", "https://api.deepseek.com/anthropic",
                                    {"deepseek-v4-flash", "deepseek-v4-pro"},
                                    "deepseek-v4-flash", 256000);  // 默认 256k,用户可在模型管理勾选 1M
            c.disable_thinking = true;  // DeepSeek 的 Anthropic 端点默认开 thinking,必须关掉
            return c;
        }(),
        anth("zhipu", "智谱 GLM", "https://open.bigmodel.cn/api/anthropic",
             {"glm-4.6", "glm-4.5"},
             "glm-4.6", 256000),
        anth("qwen", "通义千问(百炼)", "https://dashscope.aliyuncs.com/apps/anthropic",
             {"qwen3-max", "qwen3-coder-plus", "qwen-plus"},
             "qwen3-max", 256000),
        anth("kimi", "Kimi", "https://api.moonshot.cn/anthropic",
             {"kimi-k2.5", "moonshot-v1-128k"},
             "kimi-k2.5", 256000),
        anth("minimax", "MiniMax", "https://api.minimaxi.com/anthropic",
             {"MiniMax-Text-01"},
             "MiniMax-Text-01", 256000),
        anth("siliconflow", "SiliconFlow", "https://api.siliconflow.cn",
             {"deepseek-ai/DeepSeek-V3", "Qwen/Qwen3-235B-A22B"},
             "deepseek-ai/DeepSeek-V3", 256000),
        anth("openrouter", "OpenRouter(Anthropic 模式)", "https://openrouter.ai/api",
             {"anthropic/claude-opus-4.8", "deepseek/deepseek-v4-flash"},
             "anthropic/claude-opus-4.8", 256000),

        // ── OpenAI 兼容:chat completions,覆盖 OpenAI/Gemini/Ollama 等 ──
        oai("openai", "OpenAI 官方", "https://api.openai.com/v1",
            {"gpt-5", "gpt-5-mini", "gpt-4o"},
            "gpt-5", 256000),
        oai("gemini", "Gemini(OpenAI 兼容模式)", "https://generativelanguage.googleapis.com/v1beta/openai",
            {"gemini-2.5-pro", "gemini-2.5-flash"},
            "gemini-2.5-pro", 256000),
        oai("ollama", "Ollama(本地)", "http://localhost:11434/v1",
            {}, "llama3.1", 256000),

        // ── 自定义模板:手动填 base_url + model,覆盖所有漏网的 ──
        []() {
            ProviderConfig c;
            c.id = "custom";
            c.name = "自定义";
            c.adapter = AdapterType::OpenAICompat;
            c.default_model = "";
            c.context_window = 256000;
            return c;
        }(),
    };
}

} // namespace hermes
