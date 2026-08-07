#pragma once

namespace hermes {

// 回合 worker 线程进入/退出时调用。Windows 空实现;Android 做
// AttachCurrentThread / DetachCurrentThread —— native 线程附加到 JVM 后若不在退出前
// detach,ART 在 release 下可能 fatal 导致 app 崩溃。通用代码不含平台 #ifdef。
void platform_thread_enter();
void platform_thread_exit();

} // namespace hermes
