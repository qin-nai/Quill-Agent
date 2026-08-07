#pragma once
#include "hermes/message.hpp"
#include <nlohmann/json.hpp>

namespace hermes {

// ContentBlock / Message 的 JSON 序列化(会话持久化与 REST 接口共用)。
// wire 形状与 Claude content-block 一致:
//   {"type":"text","text":…}
//   {"type":"tool_use","id":…,"name":…,"input":{…}}
//   {"type":"tool_result","id":…,"output":…,"is_error":bool}
//   {"type":"image","media_type":…,"base64":…}

inline void to_json(nlohmann::json& j, const ContentBlock& b) {
    switch (b.type) {
        case BlockType::Text:
            j = nlohmann::json{{"type", "text"}, {"text", b.text}};
            break;
        case BlockType::ToolUse:
            j = nlohmann::json{{"type", "tool_use"}, {"id", b.tool_use_id},
                               {"name", b.tool_name}, {"input", b.tool_input}};
            break;
        case BlockType::ToolResult:
            j = nlohmann::json{{"type", "tool_result"}, {"id", b.tool_use_id},
                               {"output", b.tool_output}, {"is_error", b.is_error}};
            break;
        case BlockType::Image:
            j = nlohmann::json{{"type", "image"}, {"media_type", b.image_media_type},
                               {"base64", b.image_base64}};
            break;
    }
}

inline void from_json(const nlohmann::json& j, ContentBlock& b) {
    const std::string t = j.value("type", "text");
    if (t == "tool_use") {
        b = ContentBlock::make_tool_use(j.value("id", ""), j.value("name", ""),
                                        j.value("input", nlohmann::json::object()));
    } else if (t == "tool_result") {
        b = ContentBlock::make_tool_result(j.value("id", ""), j.value("output", ""),
                                           j.value("is_error", false));
    } else if (t == "image") {
        b = ContentBlock::make_image(j.value("media_type", ""), j.value("base64", ""));
    } else {
        b = ContentBlock::make_text(j.value("text", ""));
    }
}

inline void to_json(nlohmann::json& j, const Message& m) {
    j = nlohmann::json{{"role", m.role}, {"content", m.content}};
}

inline void from_json(const nlohmann::json& j, Message& m) {
    m.role = j.value("role", "user");
    m.content = j.value("content", std::vector<ContentBlock>{});
}

} // namespace hermes
