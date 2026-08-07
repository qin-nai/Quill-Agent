#pragma once
#include <atomic>
#include "hermes/http_transport.hpp"

namespace hermes {

// 真实出站 HTTP 传输(WinHTTP,支持 HTTPS 与流式 body 回调)。
// abort 置 true 时中断读取并返回 {status:0, error:"aborted"}。
// timeout_ms:连接/发送等超时;receive_idle_ms:流式相邻数据块之间的空闲超时(SSE 场景
// 需比 timeout_ms 长得多,避免 thinking 模型思考停顿被误判超时)。0 表示与 timeout_ms 相同。
// 流被超时/异常中断时,返回值 stream_complete=false 且 error 带原因。
HttpTransport make_http_transport(const std::atomic<bool>& abort, unsigned timeout_ms = 60000,
                                  unsigned receive_idle_ms = 0);

} // namespace hermes
