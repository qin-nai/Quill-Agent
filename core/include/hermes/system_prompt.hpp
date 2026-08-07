#pragma once
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include "hermes/skills.hpp"

namespace hermes {

// 构建一次回合的 system prompt:身份 + 项目记忆(Agent.md) + 技能清单。
// 独立模块,agent_loop 每轮直接调用,不内联拼接逻辑。
inline std::string build_system_prompt(const std::filesystem::path& workspace_root,
                                       const std::filesystem::path& user_skills_root) {
    std::string sys =
        "你是 Hermes Agent,一款由 qin-nai 开发的免费开源移动端 AI 编程助手。"
        "你可以读取、编辑工作目录内的文件来完成用户的任务。"
        "请用简体中文回复,展示代码时使用 markdown 代码块,不要使用 emoji。";

    std::error_code ec;
    const auto md = workspace_root / "Agent.md";
    if (!md.empty() && std::filesystem::exists(md, ec)) {
        std::ifstream f(md, std::ios::binary);
        std::string content((std::istreambuf_iterator<char>(f)),
                            std::istreambuf_iterator<char>());
        if (content.size() > 32768) content = content.substr(0, 32768) + "\n…(已截断)";
        if (!content.empty())
            sys += "\n\n# 项目记忆(Agent.md)\n以下内容来自工作目录 Agent.md,请遵守其中的约定:\n" + content;
    }

    auto skills = scan_skills(workspace_root, user_skills_root);
    if (!skills.empty()) {
        sys += "\n\n# 可用技能(Skills)\n需要执行某项技能时,用 read_skill 工具加载其详细内容:\n";
        int n = 0;
        for (const auto& s : skills) {
            if (++n > 30) { sys += "- …(更多技能)\n"; break; }
            sys += "- " + s.name + (s.description.empty() ? "" : ": " + s.description) + "\n";
        }
    }
    return sys;
}

} // namespace hermes
