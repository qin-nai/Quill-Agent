# Quill Agent · Android 打包

Android 端 = **WebView 壳 + JNI 桥**。本地 HTTP server 和 Core 引擎与 Windows 是**同一套 C++ 源码**，Android 只做两件事：把 server 跑起来、把网络层换掉。

**架构**：Core 引擎零改动（无 `#ifdef` 平台特判）。出站 HTTPS 不走 OpenSSL（交叉编译依赖被墙的 make/源码），改为 **JNI 桥 Java `HttpURLConnection`**，系统自带 TLS。

---

## 1. 环境依赖（本机验证过的版本）

| 组件 | 版本 | 位置 |
|---|---|---|
| Android SDK | — | `E:/Android/Sdk`（见 `local.properties`） |
| NDK | 30.0.15729638 | SDK 自带 |
| JDK | 17 | `E:/Android/jdk-17.0.20+8`（见 `gradle.properties` 的 `org.gradle.java.home`） |
| Gradle | 8.11.1 | 腾讯云镜像下载，勿从 GitHub 拉 |
| AGP | 8.7.3 | 阿里云 maven 镜像（`settings.gradle` / `build.gradle` 已配好） |

> 本机网络环境 GitHub 被墙、无 make——所有依赖走国内镜像，不要直接访问 google()/gradlePluginPortal() 原站。

## 2. 构建

```bash
cd android
E:/Android/gradle-8.11.1/bin/gradle.bat assembleDebug
# → android/app/build/outputs/apk/debug/app-debug.apk
```

产物仅 **arm64-v8a**（`abiFilters 'arm64-v8a'`），约 6~7MB。

## 3. 工程配置速查

| 项 | 值 |
|---|---|
| 包名 | `com.hermes.agent`（`applicationId`） |
| 版本 | `versionCode 1` / `versionName "1.0"`（**仅 debug 签名**，正式分发需 release 签名） |
| SDK 版本 | compileSdk 35 · minSdk 24 · targetSdk 35 |
| 权限 | `INTERNET` + `usesCleartextTraffic="true"`（localhost HTTP 必需） |

## 4. 目录结构

```
android/
├─ CMakeLists.txt                 NDK 构建 native 引擎 (.so)
├─ server_android.cpp             JNI 入口:启动本地 server (与 Windows main.cpp 同一套 server 源码)
├─ http_transport_android.cpp     实现 make_http_transport:JNI → Java HttpURLConnection
├─ platform_thread_android.cpp    线程 enter/exit hook (native 线程 attach/detach, 防 ART fatal)
├─ crash_handler.cpp/.hpp         native 崩溃处理
├─ build.gradle / settings.gradle / gradle.properties / local.properties
└─ app/src/main/
   ├─ AndroidManifest.xml         Quill Agent 主 Activity + WorkspaceFileProvider
   ├─ assets/webui/               内置 WebUI 副本 (MainActivity 启动时解压)
   └─ java/com/hermes/agent/
      ├─ MainActivity.java        解压 assets/webui → 后台线程起 localhost:8090 → WebView 加载
      ├─ HermesHttp.java          HttpURLConnection 实现, JNI 调用, 缓存类引用 (classloader 修复)
      ├─ FileBridge.java          WebView 暴露给 JS 的桥: 长按文件 → "用其他应用打开"
      └─ WorkspaceFileProvider.java  content:// 暴露工作区文件 (自实现, 严格限定工作区内)
```

## 5. JNI 桥关键点（踩过的坑）

- **`FindClass` 在 native 线程找不到 app 类**（classloader 问题）→ `MainActivity.initNative()` 在主线程缓存 `HermesHttp` 全局类引用 + 方法 ID，worker 线程直接用缓存。
- **JNI 引用释放后继续使用**（`DeleteLocalRef` 后又调用）→ 改用缓存类，不再释放。
- **native 回合线程 attach 后不 detach** → 退出时 ART 可能 fatal → `platform_thread_enter/exit` hook（Windows 空实现，Android 做 attach/detach）。

## 6. 已知限制

- APK 仅 debug 签名
- 移动网络切换（WiFi↔蜂窝）不恢复流式
- `run_script`（QuickJS 沙盒）在 Android 上**完整可用**（NDK clang 编译通过），Windows 才降级

详细构建排障见 [../docs/TROUBLESHOOTING.md](../docs/TROUBLESHOOTING.md)。
