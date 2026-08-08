# Quill Agent · HTTP API 接口

本地 HTTP server(`cpp-httplib`),前端 WebUI 通过 REST + SSE 驱动。默认端口 `8090`。

**通用约定**:
- 请求体为 JSON(`Content-Type: application/json`)
- 错误响应:`{"error": "<原因>"}`,配合非 200 状态码(400/401/403/404/409/413/502)
- `data/` 目录下的文件接口一律封禁(含 SQLite key 明文)

---

## 1. 健康检查

**`GET /api/health`**

```json
{ "ok": true }
```

## 2. Provider 与模型

### `GET /api/providers`

返回全部 provider 及模型列表(内置 ∪ 自定义,已剔除删除的内置模型)。

```json
[
  {
    "id": "deepseek", "name": "DeepSeek", "adapter": "claude",
    "base_url": "https://api.deepseek.com/anthropic",
    "models": ["deepseek-v4-flash", "deepseek-v4-pro"],
    "default_model": "deepseek-v4-flash",
    "context_window": 256000,
    "custom_models": [],          // 用户自定义模型
    "removed_models": [],         // 被删除的内置模型
    "model_ctx": {}               // 单模型上下文覆盖 {model: window}
  }
]
```

### `POST /api/providers/{id}/models` — 添加自定义模型

```json
{ "model": "deepseek-r1" }
```
→ `{ "ok": true }`

### `POST /api/providers/{id}/models/remove` — 删除模型

```json
{ "model": "deepseek-v4-pro" }
```
内置模型 → 记入黑名单(隐藏);自定义 → 真删。→ `{ "ok": true }`

### `POST /api/providers/{id}/models/ctx` — 单模型上下文覆盖

```json
{ "model": "deepseek-v4-flash", "context_window": 1000000 }
```
`context_window > 0` 存覆盖(勾选 1M),`= 0` 清除回默认 256k。→ `{ "ok": true }`

### `POST /api/providers/{id}/key` — 保存 API Key

```json
{ "key": "sk-..." }
```
空 key 表示删除。→ `{ "ok": true }`

### `GET /api/providers/{id}/key` — 回读已存 key

```json
{ "key": "sk-..." }
```

### `POST /api/providers/{id}/test` — 连通性检测

最小 1-token 非流式请求。请求体 `key` 可省略(用已存 key)。

```json
{ "key": "sk-..." }
```
→ `{ "ok": true, "latency_ms": 320 }`;key 无效 → 401 `{ "error": "key invalid" }`

## 3. 会话

### `GET /api/sessions`

```json
[
  { "id": "s_...", "title": "修复登录页", "provider": "deepseek",
    "model": "deepseek-v4-flash", "created_at": "2026-08-07T22:00:00" }
]
```

### `POST /api/sessions` — 创建会话

```json
{ "provider": "deepseek", "model": "deepseek-v4-flash", "title": "" }
```
→ `{ "session": { ... } }`;首条用户消息后标题自动取消息开头。

### `GET /api/sessions/{id}/messages` — 拉取历史

```json
{ "id": "s_...", "title": "...", "provider": "...", "model": "...",
  "created_at": "...",
  "messages": [ { "role": "user", "content": [ { "type": "text", "text": "..." } ] } ] }
```

### `POST /api/sessions/{id}/messages` — 发送消息(SSE 流式)

```json
{ "text": "帮我修一下登录页", "depth": 3, "model": "deepseek-v4-flash" }
```
- `depth`:思考深度(1~5)
- `model`:模型栏切换 → 覆盖会话模型并持久化
- 响应 `Content-Type: text/event-stream`,chunked;15s 无事件发 `: ping` 心跳
- 该会话已有回合在跑 → 409 `session busy`
- 未配置 key → 400

**SSE 事件**(`event: <name>` + `data: <json>`):

| 事件 | data | 说明 |
|---|---|---|
| `text_delta` | `{text}` | 文本增量 |
| `tool_use` | `{id, name, input}` | LLM 请求执行工具 |
| `confirm_request` | `{id, name, input}` | 需人工确认的工具调用 |
| `tool_result` | `{id, name, ok, output}` | 工具执行结果 |
| `done` | `{}` | 回合正常结束 |
| `error` | `{message}` | 出错(LLM/内部/响应中断) |
| `usage` | `{in, out, context_used, context_window}` | 回合 token 统计 |

### `POST /api/sessions/{id}/confirm` — 确认工具执行

收到 `confirm_request` 后,由用户决定并应答:

```json
{ "allow": true, "reason": "" }
```
→ `{ "ok": true }`;worker 线程继续。拒绝时 `allow: false`,`reason` 可选。

### `POST /api/sessions/{id}/stop` — 停止生成

→ `{ "ok": true }`;abort 回合,SSE 流随后结束。

### `DELETE /api/sessions/{id}` — 删除会话

→ `{ "ok": true }`;不存在 → 404 `{ "error": "session not found" }`;
该会话有回合在跑 → 409 `{ "error": "session busy" }`(前端运行中隐藏删除入口)。

## 4. 工作区文件

### `GET /api/files/tree` — 文件树

```json
{ "root": [ { "name": "src", "type": "dir", "path": "src",
              "children": [ { "name": "main.cpp", "type": "file", "path": "src/main.cpp", "size": 1024 } ] } ] }
```
排除 `data/`、`.git`、`build`、`node_modules`,深度上限 8。

### `GET /api/files/content?path=<rel>` — 读文件内容

→ `{ "ok": true, "content": "..." }`;越界 → 400,超 1MB → 413,`data/` 下 → 403。

---

## 5. 关键流程

**对话回合**:
```
前端 POST messages(SSE) → worker 线程跑 AgentLoop
  → 流式 text_delta / tool_use / confirm_request / tool_result / usage → done / error
  → 结束后发 close,SSE 流关闭
```

**人工确认**:
```
LLM 请求 write_file(requires_confirmation=true)
  → SSE 发 confirm_request
  → 前端弹窗 → POST /confirm {allow, reason}
  → worker 继续执行或拒绝
```

**模型栏切换**:
```
POST messages 带 model → server 覆盖会话模型并持久化
  → 后续请求用新模型
```
