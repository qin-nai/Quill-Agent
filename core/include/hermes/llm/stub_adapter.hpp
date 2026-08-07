#pragma once
#include "hermes/llm/adapter.hpp"

namespace hermes {

// M1 调试用:硬编码「一次 tool 调用 + 一次最终回答」,验证 Agent Loop 能跑通。
// 不发起任何网络请求。
class StubAdapter : public IProviderAdapter {
public:
    void stream(const ProviderConfig& cfg, const LlmRequest& req,
                const EventCallback& on_event) override;
};

} // namespace hermes
