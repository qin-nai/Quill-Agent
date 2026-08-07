#pragma once
#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iterator>
#include "hermes/skills.hpp"
#include "hermes/tool.hpp"

namespace hermes {

// 读取指定技能的 SKILL.md 全文与辅助文件列表。技能清单在系统提示中给出。
class ReadSkillTool : public ITool {
public:
    ReadSkillTool(std::filesystem::path workspace_root, std::filesystem::path user_skills_root)
        : ws_(std::move(workspace_root)), usr_(std::move(user_skills_root)) {}

    ToolSchema schema() const override {
        return {
            "read_skill",
            "读取指定技能(Skill)的 SKILL.md 完整内容与辅助文件列表。技能清单在系统提示里给出,"
            "需要执行某项技能时先调用本工具获取其详细步骤。",
            {
                {"type", "object"},
                {"properties", {
                    {"skill", {{"type", "string"}, {"description", "技能名(见系统提示的可用技能列表)"}}},
                }},
                {"required", {"skill"}}
            }
        };
    }

    ToolResult execute(const nlohmann::json& input) override {
        const std::string want = input.at("skill").get<std::string>();
        auto skills = scan_skills(ws_, usr_);
        const SkillInfo* hit = nullptr;
        for (const auto& s : skills)
            if (ci_equal(s.name, want)) { hit = &s; break; }
        if (!hit) {
            std::string known;
            for (const auto& s : skills) known += "- " + s.name + "\n";
            return {"错误:未知技能 " + want + ",可用技能:\n" + known, true};
        }
        std::ifstream f(hit->dir / "SKILL.md", std::ios::binary);
        std::string body((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        if (body.size() > 20000) body = body.substr(0, 20000) + "\n…(已截断)";
        std::string out = body;
        if (!hit->helper_files.empty()) {
            out += "\n\n辅助文件:\n";
            for (const auto& hf : hit->helper_files) out += "- " + hf + "\n";
        }
        return {out, false};
    }

private:
    static bool ci_equal(const std::string& a, const std::string& b) {
        if (a.size() != b.size()) return false;
        for (size_t i = 0; i < a.size(); ++i)
            if (std::tolower(static_cast<unsigned char>(a[i])) !=
                std::tolower(static_cast<unsigned char>(b[i])))
                return false;
        return true;
    }
    std::filesystem::path ws_, usr_;
};

} // namespace hermes
