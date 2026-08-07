#pragma once
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace hermes {

// Skills 子系统:扫描技能目录并解析 SKILL.md frontmatter。独立模块,不依赖引擎其余部分。

struct SkillInfo {
    std::string name;
    std::string description;
    std::filesystem::path dir;
    std::vector<std::string> helper_files;  // 技能目录内 SKILL.md 之外的文件(相对,posix)
};

// 解析 SKILL.md:开头的 `--- ... ---` frontmatter 块内取 name:/description:(单行,可带引号)。
// 容忍 UTF-8 BOM 与 CRLF 换行。非合法 frontmatter 返回 false。
inline bool parse_skill_sk_md(const std::string& content, SkillInfo& out) {
    size_t pos = 0;
    if (content.size() >= 3 && (unsigned char)content[0] == 0xEF &&
        (unsigned char)content[1] == 0xBB && (unsigned char)content[2] == 0xBF)
        pos = 3;  // 跳过 BOM
    const size_t first_nl = content.find('\n', pos);
    if (first_nl == std::string::npos) return false;
    std::string first = content.substr(pos, first_nl - pos);
    if (!first.empty() && first.back() == '\r') first.pop_back();
    if (first != "---") return false;

    const size_t close = content.find("\n---", first_nl + 1);
    if (close == std::string::npos) return false;
    const std::string front = content.substr(first_nl + 1, close - (first_nl + 1));

    const auto grab = [&](const std::string& key) -> std::string {
        const std::string pat = key + ":";
        size_t p = 0;
        while ((p = front.find(pat, p)) != std::string::npos) {
            const size_t line_start = (p == 0) ? 0 : front.rfind('\n', p - 1) + 1;
            if (line_start == p) {  // key 必须是行首
                size_t val = front.find('\n', p);
                if (val == std::string::npos) val = front.size();
                std::string v = front.substr(p + pat.size(), val - p - pat.size());
                if (!v.empty() && v.back() == '\r') v.pop_back();
                const size_t b = v.find_first_not_of(" \t\"'");
                const size_t e = v.find_last_not_of(" \t\"'");
                if (b == std::string::npos) return "";
                v = v.substr(b, e - b + 1);
                return v;
            }
            p += pat.size();
        }
        return "";
    };

    out.name = grab("name");
    out.description = grab("description");
    return true;
}

// 扫描工作区与用户级技能目录(各自 `<root>/.hermes/skills`),按 name 去重(workspace 优先)。
inline std::vector<SkillInfo> scan_skills(const std::filesystem::path& workspace_root,
                                          const std::filesystem::path& user_skills_root) {
    std::vector<SkillInfo> out;
    std::error_code ec;

    const auto add_dir = [&](const std::filesystem::path& root) {
        if (root.empty() || !std::filesystem::exists(root, ec)) return;
        for (auto it = std::filesystem::directory_iterator(root, ec);
             !ec && it != std::filesystem::directory_iterator{}; it.increment(ec)) {
            if (!it->is_directory(ec)) continue;
            const auto sk = it->path() / "SKILL.md";
            if (!std::filesystem::exists(sk, ec)) continue;
            std::ifstream f(sk, std::ios::binary);
            const std::string content((std::istreambuf_iterator<char>(f)),
                                      std::istreambuf_iterator<char>());
            SkillInfo info;
            info.dir = it->path();
            if (!parse_skill_sk_md(content, info)) continue;
            if (info.name.empty()) info.name = it->path().filename().string();
            for (auto e = std::filesystem::directory_iterator(info.dir, ec);
                 !ec && e != std::filesystem::directory_iterator{}; e.increment(ec)) {
                const std::string n = e->path().filename().string();
                if (n == "SKILL.md") continue;
                info.helper_files.push_back(n);
            }
            out.push_back(std::move(info));
        }
    };

    add_dir(workspace_root / ".hermes" / "skills");
    if (!user_skills_root.empty() && user_skills_root != workspace_root / ".hermes" / "skills") {
        add_dir(user_skills_root);
    }

    std::vector<SkillInfo> dedup;
    for (auto& s : out) {
        bool dup = false;
        for (const auto& d : dedup)
            if (d.name == s.name) { dup = true; break; }
        if (!dup) dedup.push_back(std::move(s));
    }
    return dedup;
}

} // namespace hermes
