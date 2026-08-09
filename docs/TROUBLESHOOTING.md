# Quill Agent 常见问题排查

> 按"症状 → 原因 → 解决"排列。大部分坑来自 [DEV_LOG.md](DEV_LOG.md) 的开发实录，90% 的问题集中在**构建环境**和 **LLM 网络**两类。

---

## 一、构建（Windows）

### 1. `cmake --build build` 报 QuickJS 编译错误（C2440 等）
- **原因**：MSVC C 编译器无法编译 QuickJS（NaN-boxing 的 `JSValue=uint64` cast 系统性报 C2440，还有 pthread/Atomics 依赖）。这是**已知限制**，非配置问题。
- **解决**：Windows 使用降级实现——`run_script_bridge_stub.c` 返回"Windows 不可用"提示。构建应自动选到 stub，若你手动把 `run_script_bridge.c` 加进了构建，去掉它：
  - `run_script` 工具在 Windows 上会提示降级；**Android（NDK clang）原生编译通过，功能完整**。

### 2. 构建时找不到 httplib / json.hpp / sqlite3
- **原因**：第三方库已 vendored，路径依赖 CMake `third_party` 变量。
- **解决**：按 README 从 `core/` 目录执行 `cmake -S core -B build`，不要从项目根执行；检查 `core/third_party/` 下三个子目录是否完整。

### 3. `ctest` 找不到测试
- **原因**：没在 build 目录跑，或没配 `--test-dir`。
- **解决**：`ctest -C Release --test-dir build`。

---

## 二、构建（Android）

### 4. Gradle 下载慢 / 下载失败 / 卡住
- **原因**：GitHub 被墙、Maven Central 直连慢。本机网络环境无 make、GitHub 不可达。
- **解决**：全走国内镜像——
  - Gradle 8.11.1：腾讯云镜像（见 `android/` 内说明）
  - AGP 8.7.3：阿里云 maven 镜像（`repositories` 里加 maven 阿里云源）
  - 不要从 GitHub 拉依赖

### 5. 报 SDK / NDK / JDK 版本问题
- **原因**：Quill Agent 需要 Android SDK + NDK 30 + JDK 17 + Gradle 8.11。
- **解决**：核对 `android/local.properties` 的 `sdk.dir`；NDK 用 30；JDK 17。版本不符会报硬错误。

### 6. 构建成功但 APK 装不上 / 崩
- **原因**：APK 目前仅 **debug 签名**。
- **解决**：调试安装用 `adb install` 即可；正式分发需配 release 签名（已知限制，未做）。

### 7. 填 API Key 后"测试连接"失败
- **原因**：key 无效，或 provider 端点网络不可达（被墙/超时）。
- **解决**：先在 Windows server 上对同一 key 跑 `POST /api/providers/{id}/test` 排除 key 问题；再看端点是否可达（如 `api.deepseek.com`）。

### 8. 对话闪退（老版本）
- **原因**：早期版本的 Android JNI 崩溃——`FindClass` 在 native 线程找不到 app 类、`DeleteLocalRef` 后引用仍被使用、native 线程 attach 后不 detach 导致 ART fatal。
- **解决**：升级到包含阶段九修复的版本（`initNative()` 缓存类引用 + `platform_thread_enter/exit` hook）。若仍闪退，抓 `adb logcat` 的 native crash 栈。

### 9. 移动网络切换（WiFi↔蜂窝）后流中断
- **原因**：网络切换使连接重建，SSE 流断开。
- **解决**：**已知限制，尚未实现流式恢复**。切网后重新发送消息即可。

---

## 三、运行时（Windows server）

### 10. 起 server 后浏览器打不开 127.0.0.1:8090
- **原因**：端口被占用，或 server 没起来（参数错误）。
- **解决**：

```bash
# 看谁占用 8090（最右侧是 PID）
netstat -ano | findstr 8090
# 结束占用进程后重起（把 <PID> 换成查到的数字）
taskkill /F /PID <PID>
```

### 11. 页面能打开但一直加载 / 对话无响应
- **原因**：`--workspace` / `--data` 参数路径不存在，或 server 是旧进程。
- **解决**：确认启动命令的 `--workspace core/workspace` 路径存在；杀掉残留进程重起。

---

## 四、对话 / LLM

### 12. 回复很短或空白
- **原因**：**DeepSeek Anthropic 端点的已知坑**——默认开启 thinking，流式事件几乎全是 `thinking_delta`，而引擎只认 `text_delta`，文本被思考占用。
- **解决**：provider 需配置 `disable_thinking`（适配器发 `thinking:{type:"disabled"}`）。内置 deepseek provider 已配好；自定义端点如遇同样问题，检查是否带上了 thinking。

### 13. 测试连接报 401 `key invalid`
- **原因**：key 填错 / 已过期 / 不是该 provider 的 key。
- **解决**：重新填 key；不同 provider（deepseek/zhipu/…）的 key 不通用。

### 14. 对话中途提示"连接中断，回复可能不完整"
- **原因**：SSE 流被切断（网络抖动 / 空闲超时 / 服务重启），且不是主动 stop。
- **解决**：这是**正确行为**而非误报——断流时前端明确提示，避免静默当作成功。重新发送消息继续。流式空闲超时已放宽到 300s（thinking 模型思考停顿可能远超 60s），一般不是超时导致。

### 15. 回复还没完就自己停了
- **原因**：旧版本把"流中断当成功"（`stream_complete` 缺失）。
- **解决**：升级版本。现版本 transport 带 `stream_complete` 标志，流被超时/中断会发 `error` 而非假装完成。

### 16. 上下文百分比不涨 / 显示不准
- **原因**：百分比基于**最近一轮请求**的 input+output 估算，是近似值，且 OpenAI 兼容端点的 usage 未接入（目前 Claude 端点生效）。
- **解决**：仅作参考。精确值需等 usage 链路完整接入（已知限制）。

---

## 五、工具

### 17. `run_script` 提示不可用
- **原因**：**Windows 是降级实现**（`run_script_bridge_stub.c`，因 MSVC 无法编译 QuickJS）。
- **解决**：Android 上 `run_script` 完整可用（NDK clang + QuickJS 沙盒）。Windows 端如需执行代码，暂只能靠 agent 推理，或用子代理。

### 18. `web_search` 搜不到 / 报错
- **原因**：`web_search` 依赖 DuckDuckGo，当前网络环境不可达（工具已就绪但实际受限）。
- **解决**：**已知限制**。后续计划接可配置搜索 API；当前可用 `web_fetch` 抓已知 URL。

### 19. MCP 工具不出现 / 调用失败
- **原因**：`data/mcp_servers.json` 配置缺失或格式错，或远端 MCP server 不可达。
- **解决**：检查配置文件里 server 的 URL/schema 是否写对；确认远端可连通（MCP 走 streamable HTTP + JSON-RPC 2.0）。

### 20. 写/改文件没有确认弹窗
- **原因**：`requires_confirmation()` 判定只在 `write_file` / `edit_file` 为真；读类工具不弹窗是正常的。
- **解决**：确认是 `write_file`/`edit_file` 调用；前端在收到 `confirm_request` 事件时才弹窗，检查弹窗是否被浏览器拦截。

---

## 六、已知限制清单

| 限制 | 影响 | 计划 |
|---|---|---|
| `web_search`（DDG）网络不可达 | 搜索工具不可用 | 接可配置搜索 API |
| OpenAI 兼容端点 usage 未接 | 上下文百分比不显示 | 待接入 |
| Context Manager 只显示不裁剪 | 超长上下文不压缩历史 | token 预算裁剪 |
| APK 仅 debug 签名 | 不可正式分发 | release 签名 |
| `run_script` Windows 降级 | Windows 不能跑脚本沙盒 | 已留 stub 接口 |
| 移动网络切换不恢复流式 | 切网后需重发消息 | 流式恢复 |

---

## 快速排查顺序

```
1. 构建失败（Windows）? → 先排除 QuickJS（用 stub），再看 third_party 是否完整
2. 构建失败（Android）? → 镜像源 / SDK·NDK·JDK 版本
3. 打不开 8090?         → netstat -ano | findstr 8090
4. 回复空/短?           → provider 是否 disable_thinking
5. 测试连接 401?        → key 是否正确 / 端点可达
6. 工具不可用?          → 查「已知限制」表，多是平台/网络限制而非 bug
```

**一句话总结**：Windows 的坑集中在 QuickJS 降级（正常）和 DDG 搜索（网络）；Android 的坑集中在环境镜像（构建）和 JNI 老版本崩溃（已修，升级即可）。
