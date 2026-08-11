# Quill Agent Launcher

一站式 Windows 部署工具 —— 自动拉取、编译、更新 [Quill-Agent](https://github.com/qin-nai/Quill-Agent) 服务端，并启动配套的 WebView 客户端。

> **适用平台**：Windows x86_64  
> **项目地址**：[启动器](https://github.com/qin-nai/Quill-Agent/tree/qa_launcher)

---

## 功能概述

- **自动拉取源码**：从 GitHub 的 `main` 分支克隆 `Quill-Agent` 服务端。
- **智能编译**：利用内置便携版 CMake 和 MSVC 工具链，一键编译 `hermes_server.exe`。
- **客户端封装**：解压 `localgui.7z` 得到 WPF + WebView2 客户端，启动后自动连接本地服务。
- **增量更新**：`git pull` 同步最新代码，自动重新编译。
- **完整性校验**：对比本地与远程差异，检测文件状态。
- **修复功能**：强制重置到远程最新，解决文件损坏或冲突。
- **卸载功能**：安全删除源码目录，保留工具和客户端。
- **完全离线工具**：自带便携版 Git 和 CMake，无需系统环境变量。

---

## 快速开始

### 首次使用（一键部署）

1. 从 [启动器](https://github.com/qin-nai/Quill-Agent/tree/qa_launcher)下载对应操作系统的启动器文件包（例如“qa_launcher_windows_x86_64”）。
2. 双击 `qa_launcher.exe`（或运行 `python main.py`）：
   - 启动器自动检查工具完整性，解压 `localgui.7z`。
   - 检测到无 `Quill-Agent-main` 目录时，自动克隆 `main` 分支。
   - 克隆完成后自动编译服务端（首次编译可能耗时 5~10 分钟）。
   - 编译成功则按钮变为“启动服务”，点击即打开客户端 GUI。

> **提示**：若网络环境较差，可手动从 [主仓库](https://github.com/qin-nai/Quill-Agent) 下载源码压缩包，解压后重命名为 `Quill-Agent-main` 并放置于启动器目录下，启动器会自动跳过克隆并直接编译。

### 日常使用

- **启动服务**：点击主按钮即可打开客户端，同时后台运行 `hermes_server.exe`。
- **更新源码**：当主仓库有更新时，点击“更新源码”拉取最新代码并重新编译。
- **修复源码**：若文件损坏或冲突，使用“修复源码”强制重置到远程最新状态，并重新编译。
- **卸载**：删除 `Quill-Agent-main` 目录（源码和编译产物），保留工具和客户端。

---

## 操作界面说明

| 元素       | 说明                                  |
| -------- | ----------------------------------- |
| 主按钮      | 动态显示状态：“下载源码”、“更新源码”、“开始编译”、“启动服务”等 |
| 下拉菜单     | 包含“验证源码完整性”、“修复源码”、“卸载”             |
| 进度条 & 标签 | 实时显示克隆、编译、解压进度                      |
| 底部信息     | 版权信息 & 版本号                          |

---

## 文件结构

./
├── PortableGit/ # 便携版 Git（自动使用）
├── cmake-4.4.2-windows-x86_64/ # 便携版 CMake
├── localgui/ # 解压后的客户端 GUI（WPF+WebView2）
├── Quill-Agent-main/ # 服务端源码（克隆后生成）
│ ├── core/
│ ├── webui/
│ └── build/Release/hermes_server.exe # 编译产物
├── qa_launcher.exe # 启动器主程序
├── upset.py # 编译脚本（供启动器调用）
└── localgui.7z # 原始压缩包（解压后自动删除）

---

## 注意事项

### 网络要求

- **GitHub 访问**：启动器需要连接 `github.com` 拉取源码。若使用代理/VPN，请确保其证书有效，否则可能因 SSL 验证失败导致克隆失败。遇到问题时，可暂时关闭代理/VPN 重试。
- **克隆/验证卡顿**：首次克隆或 `git fetch` 时，若网络延迟较高，界面可能显示“克隆尝试 1/3…​”或进度条静止 **1~3 分钟**，这是正常现象，请耐心等待，不要强制关闭程序。若多次超时失败，建议更换网络环境（例如切换至手机热点）再试。

### 常见问题排查

| 现象                      | 解决方案                                                                                                        |
| ----------------------- | ----------------------------------------------------------------------------------------------------------- |
| 启动服务后客户端白屏或提示“无法连接”     | 关闭客户端，点击“验证源码完整性”或“修复源码”后重新编译，再启动。                                                                          |
| 编译失败（`cmake.exe` 报错）    | 检查 `cmake-4.4.2-windows-x86_64\bin\cmake.exe` 是否存在，以及 `Quill-Agent-main\core\CMakeLists.txt` 是否完整。          |
| 找不到 `hermes_server.exe` | 执行“修复源码”强制重新编译。                                                                                             |
| 无法删除 `Quill-Agent-main` | 关闭所有占用该目录的进程（如服务端、IDE）或对当前使用的设备进行重启任务，再尝试“卸载”。                                                              |
| 手动放置源码后启动器仍提示下载         | 确保文件夹名称为 `Quill-Agent-main`（与仓库名一致），且包含 `.git` 子目录（若从压缩包解压则无 `.git`，启动器会识别为“非 Git 仓库”并重新克隆）。建议直接使用启动器的克隆功能。 |

### 兼容性与更新策略

- **操作系统**：仅支持 Windows 10/11 x86_64。
- **工具依赖**：启动器自带便携版 Git (2.x) 和 CMake (4.4.2)，无需系统安装。
- **服务端更新**：若主仓库更新导致启动器无法正常编译或运行，请前往 [启动器](https://github.com/qin-nai/Quill-Agent/tree/qa_launcher) 下载最新版本，或参考最新服务端配置手动调整 `upset.py` 中的编译参数。

---

## 高级用法

### 纯命令行模式（调试用）

若不想通过 GUI，可直接在启动器目录下运行 Python 脚本：

```bash
python main.py
```


### 自定义编译参数

编辑 `upset.py` 中的 `build()` 函数，可修改 CMake 配置（如生成目录、构建类型等）。



贡献与反馈
-----

* 主仓库：[qin-nai/Quill-Agent](https://github.com/qin-nai/Quill-Agent)

* 启动器分支：[qa_launcher](https://github.com/qin-nai/Quill-Agent/tree/qa_launcher)



许可
--

本项目遵循主仓库的许可
```
