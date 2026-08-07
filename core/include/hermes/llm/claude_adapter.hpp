#pragma once
#include "hermes/http_transport.hpp"
#include "hermes/llm/adapter.hpp"

namespace hermes {

// Claude 原生 Messages API(参考协议,消息几乎零翻译)。
// SSE:content_block_start/delta/stop、message_delta、message_stop;tool 参数走 input_json_delta。
class ClaudeAdapter : public IProviderAdapter {
public:
    explicit ClaudeAdapter(HttpTransport transport) : transport_(std::move(transport)) {}
    void stream(const ProviderConfig& cfg, const LlmRequest& req,
                const EventCallback& on_event) override;

private:
    HttpTransport transport_;
};

} // namespace hermes
