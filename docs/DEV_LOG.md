# Quill Agent · 开发日志

记录架构演进与关键决策。按阶段组织,重点写"为什么这么做"。

---

## 阶段一 · 消息模型与引擎骨架

**决策:消息模型参照 Claude API 的 content block 结构。**

Claude API 把工具调用直接整合进 `user`/`assistant` 消息里,消息由 `text`/`image`/`tool_use`/`tool_result` 几种 content block 组成,不设单独的 tool 角色。引擎照抄这套设计(`core/include/hermes/message.hpp`),好处:

- 与 Claude 原生协议 1:1 映射,Claude adapter 零转换损耗
- OpenAI 兼容端点在 adapter 层做"tool 结果 → 下一条 `user` 消息"的拆分,语义清晰
- 所有 provider 共用同一套内部消息格式,provider 中立

同时定义 `IProviderAdapter`(`llm/adapter.hpp`),把"网络 + SSE 流式解析 + wire 协议翻译"与引擎解耦;`HttpTransport` 注入式抽象让网络层可换平台实现。

## 阶段二 · 基础链路打通(Windows)

打通 **AgentLoop → LLM(Claude/OpenAI 双 adapter,SSE 流式)→ 工具执行 → 回填** 的完整循环,配上:

- `cpp-httplib` 本地 HTTP server,SSE chunked 流式输出回合事件
- SQLite 持久化(会话/Key/自定义模型)
- 原生 vanilla-JS WebUI(零依赖,不引框架)

**关键决策:回合跑在独立 worker 线程 + 32 线程池。** SSE 回合整个阻塞一个池线程;`confirm`/`stop` 请求必须由另一池线程应答,否则单线程串行会死锁。线程池必须够大(32)。

## 阶段三 · 前端体验打磨

- 设置抽屉:三级表单收纳、卡片紧凑化、可展开列表
- 对话字体/字号可调(系统/等宽 × 小/中/大/自定义)
- 工具调用卡片:同一回合合并分组、单行紧凑化
- 会话自动命名:首条消息开头做标题,不再固定"新会话"
- 模型管理:自定义模型增删、内置模型可删(黑名单)、单模型上下文覆盖

## 阶段四 · Claude Code 能力对齐(模块化重构)

补齐 Claude Code 的主要能力,**按模块化原则拆到独立文件,聚合点只做组合**:

| 能力 | 模块 |
|---|---|
| 网页抓取 / 搜索 | `tools/web_fetch_tool.hpp`、`tools/web_search_tool.hpp` |
| Skills | `skills.hpp`(解析/扫描)+ `tools/read_skill_tool.hpp` |
| MCP | `server/mcp_client`(streamable HTTP JSON-RPC)+ `server/mcp_tool.hpp` |
| 项目记忆 | `system_prompt.hpp`(注入 `<workspace>/Agent.md`) |
| 文件搜索 | `tools/glob.hpp`、`tools/grep_tool.hpp` |
| 待办清单 | `tools/todo_tool.hpp`(有状态+互斥锁) |
| 子代理 | `task_runner.hpp`(thread_local)+ `tools/task_tool.hpp`,嵌套上限 3 层 |

约束:`agent_loop.cpp` 只加构造参数 + 一行调 `system_prompt` + 一段安装 task runner;`app_context.cpp` 只加一行调 `tool_factory`。业务逻辑全部留在独立模块。

**代价**:`web_search` 依赖 DuckDuckGo,当前网络环境不可达,工具已就绪但实际受限(如实记录)。

## 阶段五 · 可靠性治理

针对"命中率低 / 连接中断"做了系统性排查与修复:

- **流中断被静默当成功**:transport 增加 `stream_complete` 标志,adapter 在流被超时/连接中断时发 Error 而非假装完成
- **无重试**:agent_loop 对 LLM 调用重试最多 2 次(无输出时;4xx 非限流与用户停止不重试)
- **超时过短**:LLM 流式 receive 空闲超时放宽到 300s(thinking 模型思考停顿可能远超 60s)
- **worker 异常无保护**:回合线程包 try/catch,任何异常上报并正常收尾,不泄漏 active 状态
- **子代理污染事件流**:内层回合不再发 `done`/`usage`,避免前端误判回合结束
- **前端静默断流**:SSE 被切断且非主动停止时提示"连接中断,回复可能不完整"

**发现 DeepSeek Anthropic 端点的关键坑**:默认开启 thinking,流式事件几乎全是 `thinking_delta`,而引擎只认 `text_delta` → 文本被思考占用,回复短/空。修复:provider 加 `disable_thinking` 配置,适配器发 `thinking:{type:"disabled"}`。

## 阶段六 · Token 统计

新增 usage 链路:adapter 解析 SSE 的 `message_start`/`message_delta` 的 usage → agent_loop 回合级累计 → `usage` SSE 事件 → 前端显示模型卡片上下文百分比 + 每回合消耗 token(k 单位)。

上下文默认 256k,模型管理里可勾选"1M"覆盖单模型上下文。

## 阶段七 · Android 打包(2026-08-07)

目标:点图标直接启动 WebUI,像 Windows 的 WebView2 一样。

**环境**(本机无 Android SDK,网络受限——GitHub 被墙、无 make,全部走国内镜像):
- Android SDK + NDK 30 + cmake/ninja(SDK 自带)+ JDK 17 + Gradle 8.11.1(腾讯云镜像)+ AGP 8.7.3(阿里云 maven 镜像)

**架构决策:Core 引擎零改动,Android 适配全在 `android/` 目录。**
- 出站 HTTPS **不走 OpenSSL**(交叉编译依赖被墙的 make/源码),改为 **JNI 桥 Java `HttpURLConnection`**,系统自带 TLS
- `http_transport_android.cpp` 实现 `make_http_transport`,复用与 Windows 相同的跨平台声明头(`http_transport_winhttp.hpp` 本身是纯声明)
- `server_android.cpp` 提供 JNI 入口启动本地 server(与 Windows `main.cpp` 同一套 server 源码)
- MainActivity:解压 assets/webui → 后台线程起 `localhost:8090` → WebView 加载

**产出**:`HermesAgent.apk`(6.2MB,arm64-v8a,native 引擎 .so 4.7MB)。

**留白**(手册提及但未做,待定):
- Context Manager token 预算裁剪与历史压缩(当前上下文仅显示不裁剪)
- `run_script` 受限执行(移动端安全方案:脚本沙盒 / 远程执行,需产品决策)
- 移动网络切换(wifi↔蜂窝)时流式恢复

## 阶段八 · QuickJS 脚本沙盒

Android 沙盒无法跑 bash/任意 shell。用户选定 **QuickJS 内嵌沙盒**:引擎内嵌一个 JS 解释器,`run_script` 工具让 agent 真写并执行受限 JS(算法/文本/数据处理)。

- **沙盒能力**(MVP 只读):注入 `readFile`/`listDir`/`glob`(全部限定在工作目录内,防 `..` 逃逸)+ `console.log/error` 捕获;不暴露写/网络/模块/进程。护栏:64MB 内存 + 5s 超时中断。
- **C 桥架构**:QuickJS 的 `quickjs.h` 用大量 C99 特性(designated initializer 等),被 C++ include 会编译失败 → 沙盒核心放 `.c`(`run_script_bridge.c`),`run_script_tool.cpp` 只调纯 C 接口。
- **MSVC 无法编译 QuickJS**:QuickJS 的 NaN-boxing(`JSValue=uint64`)cast 在 MSVC C 编译器下系统性报 C2440,还有 pthread(Atomics)依赖。**Android(NDK clang)原生编译通过**;**Windows 用降级实现**(`run_script_bridge_stub.c` 返回提示)。为此给 QuickJS 打了兼容补丁(禁用 Atomics、Windows time 替代、CONFIG_VERSION 兜底)。
- 产出:`HermesAgent.apk` 含 QuickJS 引擎,`run_script` 工具已注册。

## 阶段九 · 移动端稳定性与体验修复(2026-08-07)

**Android JNI 崩溃修复**(用户实测 K70U:填 key 后测试连接失败、对话闪退):
- `FindClass` 在 native 线程找不到 app 的类(classloader 问题)→ 主线程 `initNative()` 缓存 `HermesHttp` 全局类引用 + 方法 ID,worker 线程直接用缓存
- JNI 引用释放后继续使用(`DeleteLocalRef(cls)` 后又调用)→ 已释放引用导致 native crash,改用缓存类
- native 回合线程 `AttachCurrentThread` 后不 detach,退出时 ART 可能 fatal → 新增跨平台 `platform_thread_enter/exit` hook(Windows 空实现,Android 做 attach/detach)

**UI 打磨**:
- 工具行为框:同一回合的多个行为合并成一个整体框(Kimi 风格),字号/内边距再次缩小,仅作轻量提示
- 文件树侧边栏:先打开抽屉再异步取数据(避免 fetch 挂起时侧边栏不显示)
- 新建会话:加防抖锁,快速连点只创建一次

## 待办 / 已知限制

- WebSearch(DDG)在当前网络不可用,建议后续接可配置的搜索 API
- OpenAI 兼容端点的 usage 统计未接(目前 Claude 端点生效)
- Android APK 仅 debug 签名;正式分发需 release 签名
- 上下文百分比为近似值(基于最近一轮请求 input+output 估算)
