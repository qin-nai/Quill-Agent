# Quill Agent

免费开源的移动端 AI 编程助手——像 Claude Code 一样,让 AI 替你读代码、改文件、跑任务,但跑在手机上。

**核心**:一套纯 C++ 引擎(`core/`),通过本地 HTTP server + 原生 WebUI(`webui/`)驱动,Android 端用 WebView 壳打包(`android/`),点图标即用。

---

## 特性

- **Agent Loop**:自动驱动「发消息 → LLM 流式回复 → 执行工具 → 回填结果 → 再问」直到完成任务
- **工具系统**:文件读写/编辑、代码搜索(glob/grep)、网页抓取、待办清单、子代理,全部注册式扩展
- **受限 JS 脚本沙盒**:`run_script` 工具内嵌 QuickJS,agent 真写并执行 JS(算法/文本/数据处理);Android 完整可用,Windows 桌面降级
- **Claude Code 对齐能力**:Skills、MCP、项目记忆(`Agent.md`)、子代理(task)、Todo、上下文 token 统计
- **多模型服务商**:内置 Claude/DeepSeek/GLM/Qwen/Kimi/MiniMax/SiliconFlow/OpenRouter/OpenAI/Gemini/Ollama,支持自定义端点与自定义模型
- **人工确认门禁**:危险操作(写/改文件)先弹窗由你确认
- **平台可移植**:引擎零平台依赖,`HttpTransport` 注入网络层(WinHTTP / Android 系统栈),同一套代码跑 Windows 与 Android
- **移动端打包**:Android APK,内置本地 server + WebView,像 Windows 的 WebView2 一样点图标即用

## 快速开始(Windows)

```bash
# 构建(需要 CMake + MSVC)
cmake -S core -B build
cmake --build build --config Release

# 启动 server(托管 webui + 引擎 API)
./build/Release/hermes_server.exe \
  --webui webui \
  --workspace core/workspace \
  --data core/workspace/data
# → http://127.0.0.1:8090

# 测试
ctest -C Release --test-dir build
```

浏览器打开 `http://127.0.0.1:8090`,在设置里填 API Key,开始对话。

## 快速开始(Android)

```bash
cd android
E:/Android/gradle-8.11.1/bin/gradle.bat assembleDebug
# → android/app/build/outputs/apk/debug/app-debug.apk
```

环境依赖:Android SDK + NDK 30 + JDK 17 + Gradle 8.11(Gradle 依赖走国内镜像,见 `android/` 内说明)。详细步骤见 [docs/ENGINE_API.md](docs/ENGINE_API.md) 与 [docs/DEV_LOG.md](docs/DEV_LOG.md)。

## 架构

```
┌──────────────────────────────────────┐
│  平台壳 (Windows hermes_server.exe / Android WebView+JNI)  │
├──────────────────────────────────────┤
│  本地 HTTP server (cpp-httplib)        │
│  └─ REST + SSE 事件流                  │
├──────────────────────────────────────┤
│  Core Engine (纯 C++,零平台依赖)        │
│  ├─ Agent Loop (agent_loop)           │
│  ├─ LLM Client + Adapter (Claude/OpenAI)│
│  ├─ Tool Registry & 工具实现            │
│  ├─ 确认门禁 (confirm_cb)              │
│  └─ Session/Key/Model Store (SQLite)   │
├──────────────────────────────────────┤
│  WebUI (vanilla JS,零依赖)             │
└──────────────────────────────────────┘
```

**设计原则**:Core 引擎不出现任何 `#ifdef` 平台特判——网络层通过 `HttpTransport` 注入(WinHTTP / Android JNI),持久化用跨平台 SQLite,消息模型完全参照 Claude API 的 content block 结构。

## 目录结构

```
core/
  include/hermes/    引擎公开接口(消息、AgentLoop、工具、LLM、Skills)
  src/               引擎实现(adapters、provider、agent_loop)
  server/            本地 HTTP server(路由、SQLite 存储、SSE、MCP client)
  third_party/       httplib、nlohmann/json、sqlite3(vendored)
  tests/             单元测试
  workspace/         默认工作区(含 Agent.md、.hermes/skills 示例)
webui/               原生 WebUI(app.js / index.html / style.css)
android/             Android 打包:JNI 桥 + WebView 壳 + Gradle 工程
docs/                开发日志、引擎接口明细、HTTP API 文档
```

## 文档

- [开发日志](docs/DEV_LOG.md) — 架构演进与关键决策
- [Core 引擎接口明细](docs/ENGINE_API.md) — 消息模型、AgentLoop、工具协议、LLM 层
- [HTTP API 接口](docs/API.md) — 前端调用的全部路由与 SSE 事件

## 许可

免费开源。由 [qin-nai](https://github.com/qin-nai) 开发。
