#pragma once
#include <string>
#include <nlohmann/json.hpp>

namespace hermes {

struct ToolSchema {
    std::string name;
    std::string description;      // 给 LLM 看:什么时候用它、参数含义、边界情况
    nlohmann::json input_schema;  // JSON Schema,约束参数结构
};

struct ToolResult {
    std::string output;
    bool is_error = false;
};

class ITool {
public:
    virtual ~ITool() = default;
    virtual ToolSchema schema() const = 0;
    virtual ToolResult execute(const nlohmann::json& input) = 0;
    virtual bool requires_confirmation() const { return false; }  // 写操作需人工确认
};

} // namespace hermes
