#pragma once
#include <cstdlib>
#include <mutex>
#include <sstream>
#include <vector>
#include "hermes/tool.hpp"

namespace hermes {

// 结构化任务清单(Claude Code TodoWrite 对应)。工具是单例、会话并发,内部加锁。
class TodoTool : public ITool {
public:
    ToolSchema schema() const override {
        return {
            "todo",
            "维护结构化任务清单,用于拆解并跟踪复杂任务。action: add 添加、update 改状态、list 列出、clear 清空。",
            {
                {"type", "object"},
                {"properties", {
                    {"action", {{"type", "string"}, {"description", "add|update|list|clear"},
                               {"enum", {"add", "update", "list", "clear"}}}},
                    {"text", {{"type", "string"}, {"description", "add 时的任务描述"}}},
                    {"id", {{"type", "string"}, {"description", "update 时目标任务 id(数字)"}}},
                    {"status", {{"type", "string"}, {"description", "update 时新状态"},
                                {"enum", {"pending", "in_progress", "completed"}}}},
                }},
                {"required", {"action"}}
            }
        };
    }

    ToolResult execute(const nlohmann::json& input) override {
        const std::string action = input.value("action", "list");
        std::lock_guard<std::mutex> lk(mtx_);
        if (action == "add") {
            const std::string text = input.value("text", "");
            if (text.empty()) return {"错误:text 不能为空", true};
            const Item it{next_id_++, "pending", text};
            items_.push_back(it);
            return {"已添加:[" + std::to_string(it.id) + "] " + it.text, false};
        }
        if (action == "clear") {
            const size_t n = items_.size();
            items_.clear();
            return {"已清空 " + std::to_string(n) + " 项", false};
        }
        if (action == "update") {
            const int id = std::atoi(input.value("id", "").c_str());
            for (auto& it : items_) {
                if (it.id == id) {
                    if (input.contains("status")) it.status = input["status"].get<std::string>();
                    if (input.contains("text")) it.text = input["text"].get<std::string>();
                    return {"已更新:[" + std::to_string(it.id) + "] " + it.status, false};
                }
            }
            return {"错误:找不到 id=" + std::to_string(id), true};
        }
        // list
        if (items_.empty()) return {"暂无待办", false};
        std::ostringstream os;
        for (const auto& it : items_) os << "[" << it.id << "] " << it.status << " " << it.text << "\n";
        return {os.str(), false};
    }

private:
    struct Item { int id; std::string status; std::string text; };
    std::mutex mtx_;
    std::vector<Item> items_;
    int next_id_ = 1;
};

} // namespace hermes
