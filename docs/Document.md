# Quill Agent 架构设计文档

> 免费开源的移动端 AI 编程代理——像 Claude Code 一样，让 AI 替你读代码、改文件、跑任务，但跑在手机上。
> 纯 C++ 引擎 + 本地 HTTP server + 原生 WebUI + Android WebView 壳。

---

## 1. 项目背景与核心场景

**问题**：Claude Code 这类 AI 编程代理只能跑在电脑上。用户想用手机直接对代码仓库下达"修登录页 / 加个功能 / 查这个 bug"这种任务，让 AI 自己读代码、改文件、反复验证，而不是人守在电脑前。

**目标**：手机上一个点图标即用的 App——内置本地 HTTP server + WebUI，AI 引擎在后台跑 Agent Loop（发消息 → 流式回复 → 执行工具 → 回填结果 → 再问），直到完成任务。桌面端（Windows）用同一套代码跑 server + 浏览器 WebUI。

---

## 2. 核心角色与边界

| 角色 | 形态 | 职责 |
|---|---|---|
| **Core 引擎** | 纯 C++（C++17），零平台依赖 | Agent Loop、工具系统、LLM 适配、确认门禁、会话/Key/模型存储 |
| **本地 HTTP server** | cpp-httplib，localhost 监听 | REST + SSE 事件流，WebUI 与引擎之间的唯一通道 |
| **WebUI** | 原生 vanilla JS，零框架 | 对话界面、文件树、工具卡片、确认弹窗、设置 |
| **平台壳** | Windows exe / Android WebView+JNI | 起本地 server、加载 WebUI、注入网络层 |

**核心约束**：Core 引擎不出现任何 `#ifdef` 平台特判。网络层通过 `HttpTransport` 注入（Windows=WinHTTP，Android=JNI 桥 Java `HttpURLConnection`），持久化用跨平台 SQLite，消息模型完全参照 Claude API 的 content block 结构。

---

## 3. 关键技术决策

### 3.1 消息模型照搬 Claude content block

Claude API 把工具调用直接整合进 `user`/`assistant` 消息里，消息由 `text`/`image`/`tool_use`/`tool_result` 几种 content block 组成，不设单独的 tool 角色。引擎照抄这套设计（`message.hpp`）：

- 与 Claude 原生协议 1:1 映射，Claude adapter 零转换损耗
- OpenAI 兼容端点在 adapter 层做"tool 结果 → 下一条 user 消息"的拆分
- 所有 provider 共用同一套内部消息格式，provider 中立

### 3.2 Agent Loop：回合驱动

```
POST messages(SSE) → worker 线程跑 AgentLoop
  → 流式 text_delta / tool_use / confirm_request / tool_result / usage
  → LLM 请求工具 → 执行 → 结果回填为下一条 user 消息 → 再问
  → LLM 不再请求工具、给出最终回答 → done → SSE 流关闭
```

- 每轮 `system` 由 `build_system_prompt(workspace_root, user_skills_root)` 构建：身份 + 项目记忆 `<workspace>/Agent.md` + 技能清单
- LLM 调用带重试：无输出时最多 2 次退避；4xx（429 除外）与用户停止不重试
- 工具结果作为下一条 `user` 消息回填（`tool_result` block）
- 需要确认的工具先发 `ConfirmRequest` 并阻塞等待 `confirm_cb`
- 子代理（内层 run）只转发文本/工具事件，不触发 `Done`/`Usage`，避免前端误判回合结束

### 3.3 确认门禁

危险操作（`write_file` / `edit_file`，`requires_confirmation() == true`）不直接执行：

```
LLM 请求 write_file
  → SSE 发 confirm_request
  → 前端弹窗（人话描述 + diff 预览 + "允许"/"拒绝" + 拒绝理由）
  → POST /api/sessions/{id}/confirm { allow, reason }
  → worker 继续执行或拒绝并回填错误
```

### 3.4 并发模型：独立 worker 线程 + 32 线程池

SSE 回合整个阻塞一个池线程；`confirm`/`stop` 请求必须由另一池线程应答，否则单线程串行会死锁。线程池必须够大（32）。回合共享状态（事件队列 + confirm 应答通道 + abort）封装在 `RunContext`。

### 3.5 传输层：`HttpTransport` 注入式抽象

```cpp
using HttpTransport = std::function<HttpResponse(const HttpRequest&, const BodyChunkCallback&)>;
```

- Windows：WinHTTP（`http_transport_winhttp.cpp`）
- Android：JNI 桥 Java `HttpURLConnection`（`http_transport_android.cpp`），系统自带 TLS，**不交叉编译 OpenSSL**
- 流被超时/中断时置 `stream_complete = false`，adapter 据此发 Error 而非假装完成

### 3.6 可靠性治理

| 问题 | 修复 |
|---|---|
| 流中断被静默当成功 | `stream_complete` 标志 → 发 Error |
| 无重试 | LLM 调用最多重试 2 次（无输出时） |
| 超时过短 | 流式 receive 空闲超时放宽到 300s（thinking 模型思考停顿远超 60s） |
| worker 异常无保护 | 回合线程包 try/catch，异常上报并正常收尾，不泄漏 active 状态 |
| 子代理污染事件流 | 内层回合不再发 done/usage |
| DeepSeek 默认开 thinking 吞掉文本 | provider 加 `disable_thinking`，适配器发 `thinking:{type:"disabled"}` |

### 3.7 脚本沙盒：QuickJS（Android 关键）

Android 沙盒无法跑 bash/任意 shell。引擎内嵌 QuickJS，`run_script` 工具让 agent 真写并执行受限 JS：

- MVP 只读：注入 `readFile`/`listDir`/`glob`（限定工作目录内，防 `..` 逃逸）+ `console.log/error` 捕获；不暴露写/网络/模块/进程
- 护栏：64MB 内存 + 5s 超时中断
- **C 桥架构**：QuickJS 的 `quickjs.h` 用大量 C99 特性，被 C++ include 会编译失败 → 沙盒核心放 `.c`（`run_script_bridge.c`），`run_script_tool.cpp` 只调纯 C 接口
- **MSVC 编译不过**：QuickJS NaN-boxing cast 在 MSVC 下系统性报 C2440 → **Android（NDK clang）原生编译通过；Windows 用降级实现**（`run_script_bridge_stub.c` 返回提示）

### 3.8 持久化：SQLite

- `sessions` 表：会话 + 消息（Message 存 JSON），首条消息自动命名、model 更新
- `keys` 表：API Key 明文存储（本地工具，数据目录文件接口一律封禁）
- `model_ctx`：自定义模型增删、内置模型删除黑名单、单模型上下文覆盖（默认 256k，可勾选 1M）

---

## 4. 软件架构：平台壳 + HTTP Server + Core 引擎 + WebUI

### 4.1 分层结构

```
┌──────────────────────────────────────────────────────┐
│  WebUI (vanilla JS, 零依赖)                           │
│  对话流 / 文件树 / 工具卡片 / 确认弹窗 / 设置           │
└──────────────────────────┬───────────────────────────┘
                           │ REST + SSE (localhost:8090)
                           ▼
┌──────────────────────────────────────────────────────┐
│  本地 HTTP server (cpp-httplib)                        │
│  routes.hpp → /api/health, providers, sessions, files │
│  SSE provider (15s 心跳) · RunContext 回合状态          │
└──────────────────────────┬───────────────────────────┘
                           ▼
┌──────────────────────────────────────────────────────┐
│  Core Engine (纯 C++, 零平台依赖)                      │
│  ├─ AgentLoop (回合驱动 + 重试 + 事件流)                │
│  ├─ LLM 层 (IProviderAdapter: Claude/OpenAI/Stub)     │
│  ├─ ToolRegistry + 内置工具 (读/写/编辑/搜索/脚本/子代理) │
│  ├─ 确认门禁 confirm_cb                                │
│  ├─ Skills / SystemPrompt (Agent.md 记忆)             │
│  └─ SessionStore/KeyStore/ModelStore (SQLite)         │
└──────────────────────────┬───────────────────────────┘
                           │ HttpTransport 注入
                           ▼
┌──────────────────────────────────────────────────────┐
│  平台壳                                                │
│  Windows hermes_server.exe / Android WebView + JNI    │
│  WinHTTP                / JNI HttpURLConnection       │
└──────────────────────────────────────────────────────┘
```

### 4.2 核心约束

- **Core 引擎不出现平台特判**：网络层 `HttpTransport` 注入，持久化 SQLite，消息模型 provider 中立
- **前端零依赖**：vanilla JS，不引框架、不引构建工具
- **模块化**：每个能力独立成文件（工具/技能/MCP/子代理/待办），聚合点（`app_context`、`tool_factory`）只做组合，业务逻辑不进 `agent_loop.cpp`

### 4.3 引擎 ↔ Server 职责划分

| 层 | 模块 | 职责 |
|---|---|---|
| 引擎公开接口 | `include/hermes/` | 消息模型、AgentLoop、工具协议、LLM、Skills |
| 引擎实现 | `src/` | agent_loop、adapter、provider |
| Server | `core/server/` | 路由、SQLite 存储、SSE、MCP client、工具装配 |

---

## 5. 项目目录结构

```
Quill Agent/
├─ core/                            Core 引擎 — 纯 C++ 零平台依赖 (开源)
│  ├─ include/hermes/               引擎公开接口 (命名空间 hermes)
│  │  ├─ message.hpp                消息模型 (content block)
│  │  ├─ agent_loop.hpp             Agent Loop (回合驱动)
│  │  ├─ http_transport.hpp         网络注入抽象 (WinHTTP/JNI 共用声明头)
│  │  ├─ provider.hpp               Provider 配置 + 内置列表
│  │  ├─ skills.hpp                 Skills 扫描/解析
│  │  ├─ system_prompt.hpp          Agent.md 记忆 + 技能清单
│  │  ├─ task_runner.hpp            子代理 (thread_local)
│  │  ├─ tool.hpp / tool_registry.hpp  工具协议 + 注册表
│  │  ├─ llm/                       adapter.hpp, claude_adapter, openai_compat, stub
│  │  └─ tools/                     内置工具 (read/write/edit/glob/grep/todo/task/…)
│  ├─ src/                          引擎实现 (agent_loop.cpp, llm/*.cpp, provider.cpp)
│  ├─ server/                       本地 HTTP server
│  │  ├─ routes.cpp                 全部路由 (REST + SSE)
│  │  ├─ session_store / keystore / model_store   SQLite 持久化
│  │  ├─ run_context.cpp            回合共享状态 + SSE provider
│  │  ├─ tool_factory.cpp           工具装配 (内置 + MCP 远程)
│  │  ├─ mcp_client.cpp             MCP streamable HTTP 客户端
│  │  ├─ http_transport_winhttp.cpp Windows 网络层
│  │  └─ run_script_bridge.c        QuickJS 沙盒 C 桥
│  ├─ cli/                          CLI 入口
│  ├─ third_party/                  httplib / nlohmann_json / sqlite3 / quickjs (vendored)
│  ├─ tests/                        单元测试
│  └─ workspace/                    默认工作区 (Agent.md + 示例)
│
├─ webui/                           原生 WebUI
│  ├─ index.html / app.js / style.css
│  └─ DESIGN.md                     设计系统 + UX 原则
│
├─ android/                         Android 打包
│  ├─ README.md                     构建环境 + JNI 桥说明
│  ├─ server_android.cpp            JNI 入口启动本地 server
│  ├─ http_transport_android.cpp    JNI 桥 Java HttpURLConnection
│  ├─ platform_thread_android.cpp   线程 attach/detach hook
│  ├─ crash_handler.cpp             崩溃处理
│  ├─ CMakeLists.txt / build.gradle Gradle + NDK 工程
│  └─ app/src/main/                 AndroidManifest + MainActivity + assets/webui + Java 桥
│     ├─ MainActivity.java          解压 webui → 后台起 server → WebView 加载
│     ├─ HermesHttp.java            HttpURLConnection JNI 桥
│     ├─ FileBridge.java            长按文件 → 用其他应用打开 (JS 桥)
│     ├─ WorkspaceFileProvider.java content:// 暴露工作区文件 (自实现, 严格限定工作区内)
│     └─ assets/webui/              内置 WebUI 副本
│
├─ docs/                            设计文档
│  ├─ Document.md                   架构设计文档 (当前文件)
│  ├─ protocol.md                   HTTP/SSE 协议规范 (状态码 + 事件时序)
│  ├─ API.md                        HTTP API 手册 (REST + SSE 端点清单)
│  ├─ ENGINE_API.md                 Core 引擎接口明细
│  ├─ TROUBLESHOOTING.md            常见问题排查 (症状→原因→解决)
│  └─ DEV_LOG.md                    开发日志 (阶段演进 + 关键决策)
│
└─ SVG/                             蓝图 / 架构图
   ├─ 项目结构骨架.html
   ├─ 引擎架构图.html
   ├─ 引擎接口蓝图.html
   └─ 前端接口蓝图.html
```

---

## 6. 数据流

### 6.1 对话回合（核心路径）

```
用户发消息
  → POST /api/sessions/{id}/messages (SSE)
  → server 检查 key / 会话 busy → RunContext 入队 → worker 线程
  → AgentLoop.run:
      构建 system (身份 + Agent.md + 技能)
      LLM stream (Claude/OpenAI 适配器, SSE 解析)
      → 流式 text_delta 转 SSE
      → 请求工具: 需确认? → confirm_request 阻塞 : 直接执行
      → 工具结果回填 → 再问
      → done / error
  → SSE 流结束, 15s 心跳兜底
```

### 6.2 人工确认

```
LLM 请求 write_file (requires_confirmation=true)
  → SSE 发 confirm_request
  → 前端弹窗 (人话描述 + diff 预览)
  → POST /confirm { allow, reason }
  → RunContext 应答通道唤醒 worker → 执行 / 拒绝
```

### 6.3 模型与 Provider

```
GET /api/providers → 内置 ∪ 自定义 (剔除黑名单)
POST /api/providers/{id}/models → 添加自定义模型
POST /api/providers/{id}/key → 保存 API Key
POST /api/providers/{id}/test → 1-token 连通性检测
POST messages 带 model → 覆盖会话模型并持久化
```

### 6.4 文件访问

```
GET /api/files/tree      → 文件树 (排除 data/.git/build/node_modules, 深度 ≤8)
GET /api/files/content   → 读文件 (越界 400, 超 1MB 413, data/ 下 403)
→ 只读预览; 编辑走对话 (write_file/edit_file + 确认门禁)
```

### 6.5 Android 启动链路

```
MainActivity → 解压 assets/webui → 后台线程起 localhost:8090 server
  → JNI 注入 HttpTransport (HttpURLConnection, 系统 TLS)
  → WebView 加载本地 WebUI
  → 崩溃处理 crash_handler 兜底
```

---

## 7. 命名

**Quill（羽毛笔）**——写作/书写工具的意象，呼应"AI 替你在代码里落笔修改"。Agent 定名强调"由 AI 自主完成一轮任务"，而非简单的对话机器人。

---

## 8. 待细化事项（TODO）

- [x] 消息模型 → content block 结构 (`message.hpp`)
- [x] Agent Loop → 回合驱动 + 事件流 (`agent_loop.hpp`)
- [x] 工具系统 → 注册式扩展 + 确认门禁 (`tool_registry.hpp`)
- [x] LLM 层 → Claude / OpenAI 兼容 / Stub 三适配器 (`llm/`)
- [x] Provider → 内置 12 家 + 自定义端点 (`provider.hpp`)
- [x] 网络注入 → HttpTransport (WinHTTP / Android JNI) (`http_transport.hpp`)
- [x] Skills + Agent.md 项目记忆 (`skills.hpp` / `system_prompt.hpp`)
- [x] MCP 客户端 → streamable HTTP JSON-RPC (`mcp_client.cpp`)
- [x] 子代理 → thread_local TaskRunner, 嵌套 ≤3 (`task_runner.hpp`)
- [x] 脚本沙盒 → QuickJS C 桥 (`run_script_bridge.c`)
- [x] 待办清单 → 有状态 + 互斥锁 (`todo_tool.hpp`)
- [x] Token 统计 → usage 链路 + 前端上下文百分比
- [x] Android 打包 → WebView 壳 + JNI 桥 (`android/`)
- [x] 移动端稳定性 → JNI 引用修复 + 线程 attach/detach
- [ ] WebSearch(DDG) 当前网络不可用 → 建议接可配置搜索 API
- [ ] OpenAI 兼容端点 usage 统计未接（目前 Claude 端点生效）
- [ ] Android APK 仅 debug 签名 → 正式分发需 release 签名
- [ ] Context Manager token 预算裁剪与历史压缩（当前上下文仅显示不裁剪）
