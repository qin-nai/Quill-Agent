#pragma once
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>
#include "hermes/tool.hpp"

namespace hermes {

class ToolRegistry {
public:
    void register_tool(std::unique_ptr<ITool> tool) {
        tools_[tool->schema().name] = std::move(tool);
    }

    std::vector<ToolSchema> schemas() const {
        std::vector<ToolSchema> out;
        out.reserve(tools_.size());
        for (const auto& [name, tool] : tools_) out.push_back(tool->schema());
        return out;
    }

    ITool* find(const std::string& name) {
        auto it = tools_.find(name);
        return it == tools_.end() ? nullptr : it->second.get();
    }

private:
    std::unordered_map<std::string, std::unique_ptr<ITool>> tools_;
};

} // namespace hermes
