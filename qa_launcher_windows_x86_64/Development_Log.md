开发日志 - Quill Agent Launcher
===========================

> 记录启动器从概念到完整实现的开发历程、关键决策与问题解决。

* * *

2026-08-10 至 2026-08-11
-----------------------

### 初始需求与设计

**背景**：需要一个 Windows 图形化启动器，自动部署 Quill-Agent 服务端，并封装客户端 GUI，简化用户操作。

**核心目标**：

* 从 GitHub 克隆/更新服务端源码。

* 自动编译（依赖 CMake + MSVC）。

* 启动本地 HTTP 服务并打开 WebView2 客户端。

* 提供文件完整性验证、修复、卸载等功能。

* 内置便携版 Git 和 CMake，无需系统环境。

* * *

### 第一阶段：基础框架搭建

**实现**：

* 基于 PyQt5 构建主界面，包含主按钮、下拉菜单、进度条。

* 创建 `VerifyThread` 使用 Git 命令检查源码状态（fetch + diff）。

* 创建 `DownloadThread` 执行 clone/pull。

* 创建 `BuildThread` 调用 `upset.py` 执行 CMake 编译。

**关键代码**：

* `main.py`：主窗口与线程管理。

* `upset.py`：编译脚本，使用 `subprocess` 调用 cmake。

**问题**：

* 验证逻辑误将所有存在的文件标记为“modified”，导致每次启动都显示“更新”。

* 原因：`_check_local_files` 中无条件将本地存在的文件加入 `modified` 列表。

**解决**：

* 改用 Git 原生命令 `git diff --quiet origin/main` 准确判断差异。

* 增加 `git fetch` 更新远程跟踪。

* * *

### 第二阶段：便携版 Git 与工具集成

**需求**：用户无需安装 Git，启动器自带便携版 Git。

**实现**：

* 添加 `get_git_exe()` 自动索引 `./PortableGit/bin/git.exe`。

* 修改所有 Git 调用使用该路径。

* 添加 `make_writable()` 函数解决文件只读属性导致的删除失败问题。

**问题**：

* Windows 下 Git 克隆的文件可能被标记为只读，导致 `shutil.rmtree` 失败。

* 首次克隆超时，且无进度反馈。

**解决**：

* 使用 `attrib -r /s /d` 递归移除只读属性。

* 使用 `Popen` 实时读取 `stderr` 输出 Git 进度信息（打印到控制台）。

* 增加重试机制（最多 3 次），超时时间设置为 600 秒。

* * *

### 第三阶段：下载工具链与 localgui 解压

**需求**：启动器本身不包含工具，但用户可手动放置 `PortableGit`、`cmake`、`localgui.7z`，启动器自动解压。

**实现**：

* `init_local_tools()` 检查工具是否存在，并解压 `localgui.7z`。

* 解压后删除压缩包，保留 `localgui/` 目录。

* 所有操作在独立 `InitThread` 中执行，避免阻塞 UI。

**问题**：

* `py7zr` 解压大文件时可能内存占用高，但未报错。

* 解压后目录权限仍为只读，需再次调用 `make_writable`。

**解决**：

* 解压完成后立即 `make_writable(extract_to)`。

* 增加异常捕获，提示用户手动放置工具。

* * *

### 第四阶段：智能编译检测与修复功能

**需求**：区分已编译和未编译状态，避免重复编译。

**实现**：

* 新增 `is_built()` 检测 `./Quill-Agent-main/build/Release/hermes_server.exe`。

* 验证完整后，若已编译则按钮显示“启动服务”，否则“开始编译”。

* “修复源码”执行 `git fetch --all` + `git reset --hard origin/main`，并强制编译。

**优化**：

* 更新/修复完成后自动触发编译，无需用户再次点击。

* * *

### 第五阶段：网络问题诊断与日志增强

**需求**：用户反馈克隆卡顿，需要详细日志定位问题。

**实现**：

* 在所有 Git 命令执行前后打印日志（`[GIT]` 前缀）。

* 捕获 `TimeoutExpired`、`CalledProcessError` 并输出详细信息。

* 在 `VerifyThread` 和 `DownloadThread` 中增加重试计数与等待提示。

**用户指导**：

* 文档明确说明网络延迟可能导致 1~3 分钟无响应，属正常现象。

* 提供手动下载源码的替代方案。

* * *

### 第六阶段：卸载功能调整

**需求**：卸载仅删除 `Quill-Agent-main`，保留工具和客户端。

**实现**：

* 修改 `on_uninstall`，只删除源码目录。

* 更新确认对话框文字，明确删除范围。

* * *

### 第七阶段：UI 优化与底部信息

**需求**：添加版权信息、版本号，放大主按钮提升易用性。

**实现**：

* 调整窗口尺寸为 600×460。

* 底部添加“Copyright (c) 2026 Liu-Zhiyan”和“v1.0”。

* 主按钮尺寸放大至 200×60。

* 使用 `QTimer.singleShot` 延迟隐藏进度条，避免信息瞬间消失。

* * *

### 第八阶段：兼容性与文档完善

**注意事项**：

* 仅支持 Windows x86_64。

* 若主仓库更新导致启动器不兼容，用户需手动下载最新启动器资源。

* 代理/VPN 可能干扰 SSL 连接，需确保证书有效或临时关闭。

**文档**：

* 编写详细的 `README.md`，涵盖快速开始、功能说明、常见问题等。

* 生成 `DEV_LOG.md` 记录开发历程。

* * *

技术栈
---

* **前端**：PyQt5（Python 3.10+）

* **编译**：便携版 CMake 4.4.2 + MSVC（需系统安装 Visual Studio Build Tools）

* **版本控制**：便携版 Git 2.x

* **解压**：`py7zr` 库（用于 7z 文件）

* **客户端 GUI**：WPF + WebView2（C#）
