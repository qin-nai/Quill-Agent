#include "crash_handler.hpp"
#include <android/log.h>
#include <dlfcn.h>
#include <signal.h>
#include <fcntl.h>
#include <unistd.h>
#include <unwind.h>
#include <cstdio>
#include <cstring>
#include <ctime>

// NDK 的 execinfo.h 不提供 backtrace(),改用 _Unwind_Backtrace(始终可用)+ dladdr 符号化。
// 信号处理器里只用 async-safe 的 open/write/snprintf,dladdr 在 bionic 上信号上下文可用。

namespace {

const int kMaxFrames = 64;
std::string g_log_path;  // 保持生命周期:JNI 传入的临时 string 不能存裸指针

struct BacktraceState {
    void** frames;
    int max;
    int count;
};

static _Unwind_Reason_Code trace_cb(struct _Unwind_Context* ctx, void* arg) {
    BacktraceState* st = static_cast<BacktraceState*>(arg);
    if (st->count >= st->max) return _URC_END_OF_STACK;
    st->frames[st->count++] = reinterpret_cast<void*>(_Unwind_GetIP(ctx));
    return _URC_NO_REASON;
}

static int capture_backtrace(void** frames, int max) {
    BacktraceState st{frames, max, 0};
    _Unwind_Backtrace(trace_cb, &st);
    return st.count;
}

void crash_handler(int sig) {
    int fd = -1;
    if (!g_log_path.empty()) {
        fd = open(g_log_path.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0644);
    }
    if (fd < 0) fd = STDOUT_FILENO;

    char head[160];
    int hl = snprintf(head, sizeof(head),
                      "\n===== crash signal=%d(%s) pid=%d time=%lld =====\n",
                      sig, strsignal(sig), (int)getpid(), (long long)time(nullptr));
    if (hl > 0) write(fd, head, (size_t)hl);

    void* frames[kMaxFrames];
    int n = capture_backtrace(frames, kMaxFrames);
    for (int i = 0; i < n; ++i) {
        char line[256];
        int l;
        Dl_info info;
        if (frames[i] && dladdr(frames[i], &info) && info.dli_sname) {
            const char* sname = info.dli_sname;
            const char* fname = info.dli_fname ? info.dli_fname : "?";
            l = snprintf(line, sizeof line, "  #%02d  0x%p  %s()+%td  [%s]\n",
                         i, frames[i], sname,
                         (const char*)frames[i] - (const char*)info.dli_saddr,
                         fname);
        } else {
            l = snprintf(line, sizeof line, "  #%02d  0x%p\n", i, frames[i]);
        }
        if (l > 0) write(fd, line, (size_t)l);
    }

    if (fd != STDOUT_FILENO) close(fd);
    __android_log_print(ANDROID_LOG_FATAL, "hermes_native", "crash signal=%d(%s)",
                        sig, strsignal(sig));

    // 恢复默认处理并重新触发,让系统生成 tombstone/logcat 完整堆栈
    signal(sig, SIG_DFL);
    raise(sig);
    _exit(1);  // 防御:万一 raise 未生效
}

} // namespace

namespace hermes {

void install_crash_handler(const std::string& log_path) {
    g_log_path = log_path;
    for (int s : {SIGSEGV, SIGABRT, SIGBUS, SIGILL, SIGFPE}) {
        signal(s, crash_handler);
    }
}

} // namespace hermes
