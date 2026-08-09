# Quill Agent · HTTP/SSE 协议规范

> 本文档定义 WebUI 与本地 HTTP server 之间的**传输层协议契约**：HTTP 状态码语义、错误响应格式、SSE 事件流时序。
> REST 端点清单见 [API.md](API.md)，本文档聚焦"协议本身"——状态怎么定义、事件以什么顺序到达、中断时怎么收尾。

---

## 1. 通信模型：双通道

Quill Agent 前后端之间只有 HTTP，分两种通道，语义不同：

| 通道 | 传输 | 用途 | 特点 |
|---|---|---|---|
| **REST** | HTTP 请求/响应 | 查询与操作（会话、provider、文件） | 一次一答，同步返回 JSON |
| **SSE** | `POST /api/sessions/{id}/messages` 的响应体 | 回合事件流 | chunked 长连接，事件驱动，15s 心跳保活 |

**约定**：请求体 JSON（`Content-Type: application/json`）；错误响应统一 `{"error": "<原因>"}` 配合非 200 状态码。

---

## 2. HTTP 状态码语义

| 状态码 | 含义 | 触发场景 |
|---|---|---|
| `200` | 成功 | 常规 GET / POST 操作 |
| `400` | 请求不合法 | 空消息体（`empty text`）、未配置 API Key、参数错误、文件越界 |
| `401` | 认证失败 | provider 测试连接时 key 无效（`key invalid`） |
| `403` | 拒绝访问 | 请求 `data/` 目录下文件（SQLite key 明文防护） |
| `404` | 资源不存在 | 会话不存在、无活跃回合（confirm/stop 找不到 run） |
| `409` | 冲突 | 会话已有回合在跑（`session busy`）、删除运行中会话 |
| `413` | 载荷过大 | 读取超 1MB 的文件 |
| `502` | 上游错误 | LLM 网关错误（透传） |

> 约定：HTTP 状态码管**请求级错误**（参数、权限、并发）；**回合内的运行错误**不走状态码，走 SSE 的 `error` 事件（见 §4.4），二者不混用。

---

## 3. SSE 传输层

**响应头**：`Content-Type: text/event-stream`、`Cache-Control: no-cache`、`Connection: keep-alive`，chunked 传输。

**帧格式**：标准 SSE 帧——`event: <name>` + `data: <json>`，事件间空一行。

**心跳**：15s 无事件时 server 发 `: ping\n\n`（注释行，客户端应忽略）。作用：穿透代理/中间层空闲超时，保持连接不被掐断。

**结束语义**：回合结束时 server 收到内部的 `Close` 事件 → 通知 worker 停止 → SSE provider 返回 false → chunked 流关闭。**客户端判定"回合结束"的唯一可靠信号是流关闭**，`done`/`error` 事件只是语义标记，不能依赖其后的流必然立即关闭。

**断流检测**：客户端若在未收到 `done`/`error`、也未主动 stop 的情况下流被切断，说明连接异常中断，回复可能不完整——前端应提示"连接中断，回复可能不完整"，而不是静默当作成功。

---

## 4. 回合事件流协议

### 4.1 事件清单

| 事件 | data | 方向 | 说明 |
|---|---|---|---|
| `text_delta` | `{ text }` | server→client | 流式文本增量 |
| `tool_use` | `{ id, name, input }` | server→client | LLM 请求执行工具 |
| `confirm_request` | `{ id, name, input }` | server→client | 需人工确认的工具调用（写/改文件） |
| `tool_result` | `{ id, name, ok, output }` | server→client | 工具执行结果 |
| `usage` | `{ in, out, context_used, context_window }` | server→client | 回合 token 统计（近似） |
| `done` | `{}` | server→client | 回合正常结束 |
| `error` | `{ message }` | server→client | 回合出错（LLM/内部/断流） |
| `: ping` | — | server→client | 心跳保活，非事件 |

### 4.2 正常回合时序（无工具）

```
POST /api/sessions/{id}/messages
→ 200, text/event-stream
  text_delta *N          (流式文本)
  usage                  (回合 token 统计)
  done
  (流关闭)
```

### 4.3 含工具回合时序

```
POST /messages → 200, SSE
  text_delta *N          (思考/解释)
  tool_use               (LLM 请求工具)
  ── 若工具需确认 ──────────────────────────
  confirm_request        ← worker 阻塞等待,不再发任何事件
    (客户端 POST /confirm {allow,reason} 应答)
    (worker 唤醒: 执行 或 拒绝)
  tool_result            {ok:true|false, output}
  ─────────────────────────────────────────
  text_delta *N          (工具结果回填后, LLM 继续)
  tool_use ...           (可能还有更多工具)
  tool_result ...
  ...
  usage
  done
  (流关闭)
```

**时序约束**：
- `confirm_request` 发出后，worker **阻塞**在该确认上，期间没有任何后续事件；SSE 靠 `: ping` 保活。直到客户端 `POST /confirm` 应答，才继续发 `tool_result`。
- 工具结果以**下一条 user 消息**（`tool_result` block）回填历史，再发起下一轮 LLM 调用，因此同一回合内 `text_delta`/`tool_use`/`tool_result` 可多轮交替。
- `usage` 在每轮（含出错轮）LLM 响应结束时发出，位于 `done`/`error` 之前；数值为"最近一轮输入 + 回合累计输出"的近似上下文占用。
- `done` **仅顶层回合**发出（子代理回合不转发 `done`/`usage`，避免前端误判回合结束）。

### 4.4 错误回合时序

```
POST /messages → 200, SSE
  text_delta *N
  usage
  error    { message }   (LLM 错误 / 内部异常 / 流中断)
  (流关闭)
```

worker 线程整体包 try/catch——任何异常都上报 `error` 事件并正常收尾，**不会让 SSE 挂死**，也不会泄漏会话的 active 状态。

### 4.5 交互端点（回合进行中）

| 端点 | 语义 |
|---|---|
| `POST /api/sessions/{id}/confirm` `{ allow, reason }` | 应答 `confirm_request`；`allow=false` + 可选 reason，工具被拒绝并回填错误 |
| `POST /api/sessions/{id}/stop` | 置 abort 标志；worker 停止后 SSE 流随之关闭。回合内 LLM 调用带 abort，中断后不会重试 |

---

## 5. 并发与状态

- 一个会话**同一时刻最多一个活跃回合**：`POST /messages` 时若会话已在 `active` 表中 → `409 session busy`。
- 回合跑在独立 worker 线程；`confirm`/`stop` 由另一池线程应答（线程池 32），避免单线程串行死锁。
- `stop` 后 server 不再接受该回合的新事件；客户端应等待 SSE 流自然关闭，而非期待 `done`。

---

## 6. 数据目录防护

`data/`（含 SQLite key 明文）下的文件在**所有文件接口一律封禁**：
- `GET /api/files/tree` 排除 `data/`、`.git`、`build`、`node_modules`
- `GET /api/files/content?path=…` 请求 `data/` 下路径 → `403`
- 工具侧由 `path_guard` 做工作目录越界防护（防 `..` 逃逸）

---

## 7. 版本与变更记录

- 当前无显式协议版本号。协议变更以 SSE 事件名/字段增减为准，前端按事件名解析、容忍未知事件。
- 变更随 [DEV_LOG.md](DEV_LOG.md) 记录。
