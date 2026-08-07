#pragma once
#include <string>

namespace hermes {

// glob 模式匹配(header-only)。语义:* 匹配非 '/' 序列;** 匹配任意(含 '/');
// ? 匹配单个非 '/' 字符;[set]/[!set] 字符类(支持 a-z 范围)。直接递归,模式通常很短。

namespace detail {

inline bool glob_match_impl(const std::string& pat, size_t p, const std::string& txt, size_t t) {
    if (p == pat.size()) return t == txt.size();
    const char pc = pat[p];
    if (pc == '*') {
        if (p + 1 < pat.size() && pat[p + 1] == '*') {  // ** 任意(含 /)
            // gitignore 语义:**/ 可匹配零层目录(跳过整个 **/)
            if (p + 2 < pat.size() && pat[p + 2] == '/' &&
                glob_match_impl(pat, p + 3, txt, t)) return true;
            for (size_t k = t; k <= txt.size(); ++k)
                if (glob_match_impl(pat, p + 2, txt, k)) return true;
            return false;
        }
        for (size_t k = t; k <= txt.size(); ++k) {       // * 不跨 /
            if (k > t && txt[k - 1] == '/') break;
            if (glob_match_impl(pat, p + 1, txt, k)) return true;
        }
        return false;
    }
    if (t == txt.size()) return false;
    if (pc == '?') return txt[t] != '/' && glob_match_impl(pat, p + 1, txt, t + 1);
    if (pc == '[') {
        const size_t end = pat.find(']', p + 1);
        if (end == std::string::npos)
            return txt[t] == '[' && glob_match_impl(pat, p + 1, txt, t + 1);
        size_t q = p + 1;
        bool negate = false;
        if (q < end && (pat[q] == '!' || pat[q] == '^')) { negate = true; ++q; }
        bool matched = false;
        for (; q < end; ++q) {
            if (q + 2 < end && pat[q + 1] == '-') {
                if (txt[t] >= pat[q] && txt[t] <= pat[q + 2]) matched = true;
                q += 2;
            } else if (pat[q] == txt[t]) {
                matched = true;
            }
        }
        if (matched == negate) return false;
        return glob_match_impl(pat, end + 1, txt, t + 1);
    }
    return pc == txt[t] && glob_match_impl(pat, p + 1, txt, t + 1);
}

} // namespace detail

inline bool glob_match(const std::string& pattern, const std::string& text) {
    return detail::glob_match_impl(pattern, 0, text, 0);
}

} // namespace hermes
