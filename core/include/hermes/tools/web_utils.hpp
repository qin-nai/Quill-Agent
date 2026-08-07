#pragma once
#include <cctype>
#include <string>

namespace hermes {

// 网页相关的纯文本工具:URL 编码/解码、HTML→纯文本。零依赖、header-only。

inline std::string url_encode(const std::string& s) {
    static const char* hex = "0123456789ABCDEF";
    std::string out;
    out.reserve(s.size());
    for (unsigned char c : s) {
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
            c == '-' || c == '_' || c == '.' || c == '~') {
            out += static_cast<char>(c);
        } else if (c == ' ') {
            out += '+';
        } else {
            out += '%';
            out += hex[c >> 4];
            out += hex[c & 15];
        }
    }
    return out;
}

inline std::string url_decode(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '+') { out += ' '; continue; }
        if (s[i] == '%' && i + 2 < s.size()) {
            const auto hexv = [](char c) -> int {
                if (c >= '0' && c <= '9') return c - '0';
                if (c >= 'a' && c <= 'f') return c - 'a' + 10;
                if (c >= 'A' && c <= 'F') return c - 'A' + 10;
                return -1;
            };
            const int h = hexv(s[i + 1]), l = hexv(s[i + 2]);
            if (h >= 0 && l >= 0) { out += static_cast<char>(h * 16 + l); i += 2; continue; }
        }
        out += s[i];
    }
    return out;
}

// HTML→可读文本:剔除 script/style 内容、块级标签转行、去其余标签、解码常见实体。
inline std::string html_to_text(const std::string& html) {
    std::string out;
    out.reserve(html.size());
    size_t i = 0;
    bool skip = false;
    const auto starts_with = [](const std::string& s, const char* p) {
        const size_t n = std::char_traits<char>::length(p);
        return s.size() >= n && s.compare(0, n, p) == 0;
    };
    const auto block_break = [&](const std::string& lt) {
        return starts_with(lt, "br") || starts_with(lt, "/p") || starts_with(lt, "/div") ||
               starts_with(lt, "/li") || starts_with(lt, "/tr") ||
               (lt.size() >= 3 && lt[0] == '/' && lt[1] == 'h' && lt[2] >= '1' && lt[2] <= '6');
    };
    while (i < html.size()) {
        if (html[i] == '<') {
            const size_t end = html.find('>', i);
            if (end == std::string::npos) break;
            std::string tag = html.substr(i + 1, end - i - 1);
            for (auto& c : tag) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            if (starts_with(tag, "script") || starts_with(tag, "style")) { skip = true; }
            else if (starts_with(tag, "/script") || starts_with(tag, "/style")) { skip = false; }
            else if (block_break(tag)) { out += '\n'; }
            i = end + 1;
            continue;
        }
        if (skip) { ++i; continue; }
        if (html[i] == '&') {
            const std::string rest = html.substr(i, 8);
            if (starts_with(rest, "&amp;")) { out += '&'; i += 5; continue; }
            if (starts_with(rest, "&lt;")) { out += '<'; i += 4; continue; }
            if (starts_with(rest, "&gt;")) { out += '>'; i += 4; continue; }
            if (starts_with(rest, "&quot;")) { out += '"'; i += 6; continue; }
            if (starts_with(rest, "&#39;") || starts_with(rest, "&apos;")) { out += '\''; i += 5; continue; }
            if (starts_with(rest, "&nbsp;")) { out += ' '; i += 6; continue; }
            if (rest[0] == '&' && rest[1] == '#') {
                const size_t semi = html.find(';', i);
                if (semi != std::string::npos && semi - i <= 8) {
                    try {
                        const long v = std::stol(html.substr(i + 2, semi - i - 2));
                        out += static_cast<char>(v);
                        i = semi + 1;
                        continue;
                    } catch (...) {}
                }
            }
            out += '&';
            ++i;
            continue;
        }
        out += html[i];
        ++i;
    }
    return out;
}

} // namespace hermes
