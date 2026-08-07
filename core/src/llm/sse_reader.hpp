#pragma once
#include <functional>
#include <string>

namespace hermes {
namespace detail {

// SSE 流解析器:输入按 chunk 到达,内部按行/字段切分。
// 每完成一个事件回调 (event 名, data 字段拼接结果)。
// 跨 chunk 的断行、\r\n 行尾、data: 多行拼接、注释行均处理。
class SseReader {
public:
    using OnEvent = std::function<void(const std::string& event, const std::string& data)>;

    explicit SseReader(OnEvent on_event) : on_event_(std::move(on_event)) {}

    void feed(const std::string& chunk) {
        buf_ += chunk;
        std::size_t pos;
        while ((pos = buf_.find('\n')) != std::string::npos) {
            std::string line = buf_.substr(0, pos);
            buf_.erase(0, pos + 1);
            handle_line(line);
        }
    }

    // 流结束时调用,清掉残留的未终结事件
    void flush() {
        if (!pending_data_.empty()) emit();
    }

private:
    void handle_line(const std::string& raw) {
        std::string line = raw;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) {  // 空行 = 事件结束
            if (!pending_data_.empty()) emit();
            return;
        }
        if (line[0] == ':') return;  // 注释行
        if (line.rfind("event:", 0) == 0) {
            pending_event_ = line.substr(6);
            if (!pending_event_.empty() && pending_event_[0] == ' ') pending_event_.erase(0, 1);
        } else if (line.rfind("data:", 0) == 0) {
            std::string v = line.substr(5);
            if (!v.empty() && v[0] == ' ') v.erase(0, 1);
            if (!pending_data_.empty()) pending_data_ += '\n';
            pending_data_ += v;
        }
        // id / retry 等字段忽略
    }

    void emit() {
        on_event_(pending_event_, pending_data_);
        pending_event_.clear();
        pending_data_.clear();
    }

    std::string buf_;
    std::string pending_event_;
    std::string pending_data_;
    OnEvent on_event_;
};

} // namespace detail
} // namespace hermes
