#pragma once
#include <memory>
#include "hermes/http_transport.hpp"
#include "hermes/llm/adapter.hpp"
#include "hermes/llm/claude_adapter.hpp"
#include "hermes/llm/openai_compat_adapter.hpp"
#include "hermes/llm/stub_adapter.hpp"

namespace hermes {

// 按 ProviderConfig.adapter 路由到对应适配器。真实网络由 HttpTransport 注入。
class LLMClient {
public:
    LLMClient(const ProviderConfig& cfg, HttpTransport transport)
        : cfg_(cfg), transport_(std::move(transport)) {}

    IProviderAdapter& adapter() {
        if (!adapter_) {
            switch (cfg_.adapter) {
                case AdapterType::Claude:
                    adapter_ = std::make_unique<ClaudeAdapter>(transport_);
                    break;
                case AdapterType::OpenAICompat:
                    adapter_ = std::make_unique<OpenAICompatAdapter>(transport_);
                    break;
                case AdapterType::Stub:
                    adapter_ = std::make_unique<StubAdapter>();
                    break;
            }
        }
        return *adapter_;
    }

private:
    ProviderConfig cfg_;
    HttpTransport transport_;
    std::unique_ptr<IProviderAdapter> adapter_;
};

} // namespace hermes
