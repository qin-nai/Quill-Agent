#pragma once
#include <string>
#include <vector>
#include <nlohmann/json.hpp>

namespace hermes {

// 内部消息模型,参考 Claude API 的 content block 结构。
// 这是跨 provider 的中立格式:适配器负责与各家的 wire 格式双向翻译。
enum class BlockType { Text, ToolUse, ToolResult, Image };

struct ContentBlock {
    BlockType type = BlockType::Text;
    std::string text;             // Text
    std::string tool_use_id;      // ToolUse / ToolResult 关联 ID
    std::string tool_name;        // ToolUse
    nlohmann::json tool_input;    // ToolUse 参数(结构化 JSON)
    std::string tool_output;      // ToolResult 结果
    bool is_error = false;        // ToolResult 是否出错
    std::string image_media_type; // Image,如 "image/png"
    std::string image_base64;     // Image 的 base64 数据

    static ContentBlock make_text(std::string t) {
        ContentBlock b; b.type = BlockType::Text; b.text = std::move(t); return b;
    }
    static ContentBlock make_tool_use(std::string id, std::string name, nlohmann::json input) {
        ContentBlock b; b.type = BlockType::ToolUse;
        b.tool_use_id = std::move(id); b.tool_name = std::move(name); b.tool_input = std::move(input);
        return b;
    }
    static ContentBlock make_tool_result(std::string id, std::string output, bool err = false) {
        ContentBlock b; b.type = BlockType::ToolResult;
        b.tool_use_id = std::move(id); b.tool_output = std::move(output); b.is_error = err;
        return b;
    }
    static ContentBlock make_image(std::string media_type, std::string base64) {
        ContentBlock b; b.type = BlockType::Image;
        b.image_media_type = std::move(media_type); b.image_base64 = std::move(base64);
        return b;
    }
};

struct Message {
    std::string role; // "user" | "assistant" | "system"
    std::vector<ContentBlock> content;
};

} // namespace hermes
