#pragma once
#include <string>

namespace hermes {

// 安装 SIGSEGV/SIGABRT 等崩溃捕获,把 backtrace 追加写到 log_path
// (同时打 logcat),方便复现后定位 native 崩溃点。
void install_crash_handler(const std::string& log_path);

} // namespace hermes
