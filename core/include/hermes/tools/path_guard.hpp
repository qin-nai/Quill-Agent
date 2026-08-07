#pragma once
#include <filesystem>
#include <optional>
#include <string>

namespace hermes {

// 把相对路径解析到工作目录内;越界(../ 逃逸)返回 nullopt。
// 用 relative() 的首段比较,比 substr(0,2)==".." 精确,不误伤 "..foo" 这类文件名。
inline std::optional<std::filesystem::path> resolve_within_root(
    const std::filesystem::path& root, const std::string& rel) {
    const auto root_canon = std::filesystem::weakly_canonical(root);
    const auto full = std::filesystem::weakly_canonical(root / rel);
    const auto rel_check = std::filesystem::relative(full, root_canon);
    if (rel_check.empty()) return std::nullopt;
    auto first = rel_check.begin();
    if (first != rel_check.end() && *first == "..") return std::nullopt;
    return full;
}

} // namespace hermes
