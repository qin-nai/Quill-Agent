# Quill Agent · Core 引擎接口明细

引擎为纯 C++(C++17),零平台依赖,头文件全部位于 `core/include/hermes/`。平台差异通过 `HttpTransport` 注入网络层。

---

## 1. 消息模型 · `include/hermes/message.hpp`

参照 Claude API 的 content block 结构:消息由若干 content block 组成,工具调用直接嵌在 `user`/`assistant` 消息里,没有单独的 tool 角色。

```cpp
enum class BlockType { Text, ToolUse, ToolResult, Image };

struct ContentBlock {
    BlockType type;
    std::string text;           // Text
    std::string tool_use_id;    // ToolUse / ToolResult 关联 ID
    std::string tool_name;      // ToolUse
    nlohmann::json tool_input;  // ToolUse 参数(结构化 JSON)
    std::string tool_output;    // ToolResult 结果
    bool is_error = false;      // ToolResult 是否出错
    std::string image_media_type, image_base64;  // Image
    // 静态工厂: make_text / make_tool_use / make_tool_result / make_image
};

struct Message {
    std::string role;                  // "user" | "assistant" | "system"
    std::vector<ContentBlock> content;
};
```

JSON 序列化(跨 provider 中立)见 `json_io.hpp`。

## 2. Agent Loop · `agent_loop.hpp`

驱动「发消息 → 流式事件 → 执行工具 → 回填 → 再问」的循环,直到 LLM 不再请求工具、给出最终回答。

```cpp
class AgentLoop {
public:
    AgentLoop(ToolRegistry& tools,
              std::filesystem::path workspace_root = {},   // 用于 Agent.md 记忆 + 技能扫描
              std::filesystem::path user_skills_root = {},
              int depth = 0);                               // 子代理嵌套深度
    bool run(const ProviderConfig& cfg, IProviderAdapter& llm,
             std::vector<Message>& history,                 // 回合历史,in/out
             std::function<bool(const ContentBlock&)> confirm_cb,  // 确认门禁
             AgentEventCallback on_event);                  // 回合级事件流
};
```

**回合级事件** `AgentEvent`(server 转 SSE 用):

| Type | 字段 | 说明 |
|---|---|---|
| TextDelta | `text` | 文本增量 |
| ToolUse | `tool_use` | LLM 请求执行工具 |
| ConfirmRequest | `tool_use` | 需要人工确认的工具调用 |
| ToolResult | `tool_id/tool_name/tool_ok/tool_output` | 工具执行结果 |
| Done | — | 回合正常结束 |
| Error | `error` | LLM 错误 |
| Close | — | SSE 流关闭 |
| Usage | `usage_in/usage_out/context_used/context_window` | 回合 token 统计 |

行为要点:
- 每轮 `req.system = build_system_prompt(workspace_root, user_skills_root)`(身份 + Agent.md + 技能清单)
- LLM 调用带重试:无输出时最多 2 次退避;4xx(429 除外)与用户停止不重试
- 工具结果作为下一条 `user` 消息回填(`tool_result` block)
- 需要确认的工具先发 `ConfirmRequest` 并阻塞等待 `confirm_cb`
- 子代理(内层 run)只转发文本/工具事件,不触发 `Done`/`Usage`

## 3. 工具协议 · `tool.hpp` + `tool_registry.hpp`

```cpp
struct ToolSchema {
    std::string name;
    std::string description;      // 给 LLM 看的自然语言描述
    nlohmann::json input_schema;  // JSON Schema
};

struct ToolResult {
    std::string output;   // 结果文本
    bool is_error = false;
};

class ITool {
public:
    virtual ToolSchema schema() const = 0;
    virtual ToolResult execute(const nlohmann::json& input) = 0;
    virtual bool requires_confirmation() const = 0;  // 需要人工确认(写/改文件)
};

class ToolRegistry {
    void register_tool(std::unique_ptr<ITool>);
    ITool* find(const std::string& name);
    std::vector<ToolSchema> schemas();   // 全部工具 schema,发给 LLM
};
```

**内置工具**(`tool_factory.cpp` 注册):

| 工具 | 输入参数 | 说明 | 需确认 |
|---|---|---|---|
| `read_file` | `path`, `offset=1`, `max_lines=500` | 读工作目录内文件 | 否 |
| `write_file` | `path`, `content` | 覆盖写入 | **是** |
| `edit_file` | `path`, `old_string`, `new_string` | 局部替换 | **是** |
| `glob` | `pattern`, `max_results=100` | 按 glob 列文件(`**` 支持) | 否 |
| `grep` | `pattern`, `path="."`, `case_sensitive=false`, `glob?`, `max_lines=100` | 正则/子串搜索 | 否 |
| `todo` | `action: add\|update\|list\|clear`, `text?`, `id?`, `status?` | 有状态待办清单 | 否 |
| `read_skill` | `skill` | 读技能 SKILL.md | 否 |
| `task` | `prompt` | 派发子代理(嵌套上限 3 层) | 否 |
| `run_script` | `script` | 受限 JS 沙盒(QuickJS),只读注入 `readFile/listDir/glob`,5s 超时+64MB 上限 | 否 |
| `web_fetch` | `url`, `max_chars=20000` | 抓网页转文本 | 否 |
| `web_search` | `query`, `max_results=8` | DDG 搜索 | 否 |
| `mcp_<server>_<tool>` | 远端 schema | MCP 远程工具(见 §8) | 否 |

## 4. LLM 层 · `llm/adapter.hpp` + `llm_client.hpp`

```cpp
struct LlmRequest {
    std::string model;
    std::vector<Message> messages;
    std::vector<ToolSchema> tools;
    std::string system;   // 顶层 system(Claude 走 system 字段,OpenAI 走首条 system 消息)
};

struct LlmEvent {          // 适配器流式事件,AgentLoop 组装
    enum class Type { TextDelta, ToolUseComplete, Done, Error };
    std::string text;
    ContentBlock tool_use;
    std::string error;
    uint32_t in_tokens = 0, out_tokens = 0;  // Done 时带 usage
};

class IProviderAdapter {
    virtual void stream(const ProviderConfig& cfg, const LlmRequest& req,
                        const EventCallback& on_event) = 0;
};

class LLMClient {          // 按 cfg.adapter 路由
    IProviderAdapter& adapter();
};
```

- `ClaudeAdapter`:原生 Messages 协议,`/v1/messages`,支持 thinking 开关、usage 解析
- `OpenAICompatAdapter`:chat completions,`/chat/completions`,tool 结果拆为 `role:"tool"` 消息
- `StubAdapter`:测试用

## 5. Provider · `provider.hpp`

```cpp
struct ProviderConfig {
    std::string id, name;
    AdapterType adapter;           // Claude | OpenAICompat | Stub
    std::string base_url;
    std::string api_key_ref;
    std::vector<std::string> models;
    std::string default_model;
    size_t context_window = 256000;      // 上下文(默认 256k,单模型可覆盖 1M)
    bool disable_thinking = false;       // DeepSeek 等默认开 thinking 的端点需关
};
std::vector<ProviderConfig> builtin_providers();
```

内置:`anthropic / deepseek / zhipu / qwen / kimi / minimax / siliconflow / openrouter / openai / gemini / ollama / custom`。

## 6. 传输层 · `http_transport.hpp`

```cpp
struct HttpRequest  { std::string method, url; std::vector<std::pair<std::string,std::string>> headers; std::string body; };
struct HttpResponse { int status; std::string error; std::string content_type; std::string mcp_session_id;
                      bool stream_complete = true; };   // 流被超时/中断为 false
using BodyChunkCallback = std::function<void(const std::string&)>;
using HttpTransport    = std::function<HttpResponse(const HttpRequest&, const BodyChunkCallback&)>;

HttpTransport make_http_transport(const std::atomic<bool>& abort,
                                  unsigned timeout_ms = 60000,      // 连接/发送
                                  unsigned receive_idle_ms = 0);     // 流式空闲超时(0=timeout_ms)
```

实现按平台:**Windows** 用 WinHTTP(`server/http_transport_winhttp.cpp`);**Android** 用 JNI 桥 Java `HttpURLConnection`(`android/http_transport_android.cpp`,系统自带 TLS,避免交叉编译 OpenSSL)。两者复用同一个跨平台声明头。

## 7. 支持模块

- **Skills** `skills.hpp`:扫描 `<workspace>/.hermes/skills` + `$USERPROFILE/.hermes/skills`,解析 SKILL.md frontmatter(BOM/CRLF 容忍)
- **System Prompt** `system_prompt.hpp`:`build_system_prompt(workspace_root, user_skills_root)` → 身份 + `<workspace>/Agent.md`(≤32KB)+ 技能清单(≤30 条)
- **子代理** `task_runner.hpp`:`thread_local` `TaskRunner`,`set/get_task_runner`

## 8. Server 模块(`core/server/`)

| 模块 | 职责 |
|---|---|
| `app_context.hpp` | 全量共享状态(SessionStore/KeyStore/ModelStore/ToolRegistry/MCP 客户端),装配工具 |
| `session_store.hpp` | 会话持久化(SQLite `sessions` 表,Message 存 JSON);首条消息自动命名、model 更新 |
| `keystore.hpp` | API Key 明文存储(SQLite `keys` 表) |
| `model_store.hpp` | 自定义模型、内置删除黑名单、单模型上下文覆盖(`model_ctx`) |
| `run_context.hpp` | 回合共享状态:事件队列 + confirm 应答通道 + abort;SSE provider(15s 心跳) |
| `tool_factory.hpp` | 工具装配:内置工具 + 读 `data/mcp_servers.json` 注册 MCP 远程工具 |
| `mcp_client.hpp/.cpp` | MCP streamable HTTP 客户端:JSON-RPC 2.0 握手 → `tools/list` → `tools/call` |
| `routes.hpp` | 全部 HTTP 路由(见 `API.md`) |
