#pragma once
#include <string>
#include <vector>

namespace hermes {

enum class AdapterType { Claude, OpenAICompat, Stub };

// 一个 provider 的完整配置。真实 API key 不进 Core,
// 只存 api_key_ref 引用,key 本体在平台 Keystore/Keychain(经桥接层)。
struct ProviderConfig {
    std::string id;            // 如 "deepseek"
    std::string name;          // 展示名
    AdapterType adapter = AdapterType::OpenAICompat;
    std::string base_url;      // Claude 适配器拼 /v1/messages,OpenAI 适配器拼 /chat/completions
    std::string api_key_ref;   // key 引用,如 "keystore:deepseek"
    std::vector<std::string> models;
    std::string default_model;
    size_t context_window = 128000;  // ContextManager token 预算参考(各厂商差异很大)
    bool disable_thinking = false;   // Claude 兼容端点默认开 thinking 的 provider(如 DeepSeek)置 true,
                                     // 让适配器发 thinking:{type:"disabled"},否则文本被思考占用
};

// 内置预设库。端点取自 CC-switch 的 claudeProviderPresets.ts(2026-08),
// 模型列表只是可编辑提示,可手动改或在设置页经 /v1/models 自动发现。
std::vector<ProviderConfig> builtin_providers();

} // namespace hermes
