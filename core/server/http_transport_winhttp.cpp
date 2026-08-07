#include "http_transport_winhttp.hpp"
#include <windows.h>
#include <winhttp.h>
#include <cstdlib>
#include <string>
#include <vector>

namespace hermes {

namespace {

struct UrlParts {
    std::string host;
    std::string path;
    int port = 443;
    bool secure = true;
};

// 解析 http(s)://host[:port]/path
bool parse_url(const std::string& url, UrlParts& out) {
    const auto scheme_end = url.find("://");
    if (scheme_end == std::string::npos) return false;
    out.secure = url.compare(0, scheme_end, "https") == 0;
    const auto path_start = url.find('/', scheme_end + 3);
    const std::string authority =
        path_start == std::string::npos ? url.substr(scheme_end + 3)
                                        : url.substr(scheme_end + 3, path_start - scheme_end - 3);
    if (authority.empty()) return false;
    out.path = path_start == std::string::npos ? "/" : url.substr(path_start);
    const auto colon = authority.rfind(':');
    if (colon != std::string::npos && authority.find('[') == std::string::npos) {
        out.host = authority.substr(0, colon);
        out.port = std::atoi(authority.substr(colon + 1).c_str());
        if (out.port <= 0) return false;
    } else {
        out.host = authority;
        out.port = out.secure ? 443 : 80;
    }
    return !out.host.empty();
}

std::wstring widen(const std::string& s) {
    if (s.empty()) return L"";
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w(n, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &w[0], n);
    return w;
}

std::string narrow(const std::wstring& w) {
    if (w.empty()) return "";
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, 0);
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), &s[0], n, nullptr, nullptr);
    return s;
}

// 查询单个响应头(自定义名),不存在返回空串。
std::string query_header(HINTERNET req, const std::wstring& name) {
    DWORD sz = 0;
    if (!WinHttpQueryHeaders(req, WINHTTP_QUERY_CUSTOM, name.c_str(),
                             WINHTTP_NO_OUTPUT_BUFFER, &sz, WINHTTP_NO_HEADER_INDEX))
        return "";
    std::wstring buf(sz, 0);
    if (!WinHttpQueryHeaders(req, WINHTTP_QUERY_CUSTOM, name.c_str(),
                             buf.data(), &sz, WINHTTP_NO_HEADER_INDEX))
        return "";
    return narrow(buf);
}

struct Handles {
    HINTERNET session = nullptr, conn = nullptr, req = nullptr;
    ~Handles() {
        if (req) WinHttpCloseHandle(req);
        if (conn) WinHttpCloseHandle(conn);
        if (session) WinHttpCloseHandle(session);
    }
};

} // namespace

HttpTransport make_http_transport(const std::atomic<bool>& abort, unsigned timeout_ms,
                                  unsigned receive_idle_ms) {
    if (receive_idle_ms == 0) receive_idle_ms = timeout_ms;
    return [&abort, timeout_ms, receive_idle_ms](const HttpRequest& hr,
                                                 const BodyChunkCallback& cb) -> HttpResponse {
        HttpResponse out;
        UrlParts url;
        if (!parse_url(hr.url, url)) { out.error = "bad url: " + hr.url; return out; }

        Handles h;
        h.session = WinHttpOpen(L"Hermes/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
        if (!h.session) { out.error = "WinHttpOpen failed"; return out; }
        WinHttpSetTimeouts(h.session, timeout_ms, timeout_ms, timeout_ms, receive_idle_ms);

        const std::wstring host_w = widen(url.host);
        h.conn = WinHttpConnect(h.session, host_w.c_str(), (INTERNET_PORT)url.port, 0);
        const std::wstring path_w = widen(url.path);
        const std::wstring method_w = widen(hr.method);
        h.req = WinHttpOpenRequest(h.conn, method_w.c_str(), path_w.c_str(), nullptr,
                                   WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                   url.secure ? WINHTTP_FLAG_SECURE : 0);
        if (!h.req) { out.error = "WinHttpOpenRequest failed"; return out; }

        DWORD redirect = WINHTTP_OPTION_REDIRECT_POLICY_ALWAYS;
        WinHttpSetOption(h.req, WINHTTP_OPTION_REDIRECT_POLICY, &redirect, sizeof(redirect));

        std::wstring hdrs;
        for (const auto& kv : hr.headers) hdrs += widen(kv.first) + L": " + widen(kv.second) + L"\r\n";

        const BOOL sent = WinHttpSendRequest(h.req,
                                             hdrs.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : hdrs.c_str(),
                                             (DWORD)(hdrs.empty() ? -1 : hdrs.size()),
                                             hr.body.empty() ? WINHTTP_NO_REQUEST_DATA
                                                             : (LPVOID)hr.body.data(),
                                             (DWORD)hr.body.size(), (DWORD)hr.body.size(), 0);
        if (!sent) { out.error = "WinHttpSendRequest failed"; return out; }
        if (!WinHttpReceiveResponse(h.req, 0)) { out.error = "WinHttpReceiveResponse failed"; return out; }

        DWORD status = 0, status_sz = sizeof(status);
        WinHttpQueryHeaders(h.req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                            WINHTTP_HEADER_NAME_BY_INDEX, &status, &status_sz,
                            WINHTTP_NO_HEADER_INDEX);
        out.status = static_cast<int>(status);
        out.content_type = query_header(h.req, L"Content-Type");
        out.mcp_session_id = query_header(h.req, L"Mcp-Session-Id");

        for (;;) {
            if (abort.load()) { out.status = 0; out.error = "aborted"; break; }
            DWORD avail = 0;
            if (!WinHttpQueryDataAvailable(h.req, &avail)) {
                // 读阶段异常:超时(thinking 模型思考停顿过久)或连接中断 → 流不完整
                const DWORD err = GetLastError();
                out.stream_complete = false;
                if (err == ERROR_WINHTTP_TIMEOUT) out.error = "receive timeout";
                else if (err == ERROR_WINHTTP_CONNECTION_ERROR) out.error = "connection error";
                else out.error = "read error " + std::to_string(err);
                break;
            }
            if (avail == 0) break;  // 正常 EOF,流完整
            std::string buf(avail, 0);
            DWORD read = 0;
            if (!WinHttpReadData(h.req, buf.data(), avail, &read)) {
                out.stream_complete = false;
                out.error = "WinHttpReadData failed";
                break;
            }
            if (read == 0) break;
            buf.resize(read);
            cb(buf);
        }
        return out;
    };
}

} // namespace hermes
