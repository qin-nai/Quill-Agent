#include <jni.h>
#include <string>
#include <filesystem>
#include <httplib.h>
#include "app_context.hpp"
#include "routes.hpp"

extern "C" void hermes_android_init(JNIEnv*);   // 缓存 HermesHttp 类引用+方法(主线程)

// MainActivity.onCreate 在主线程调用:初始化 JNI(缓存类引用),后续任何线程的 transport 都可用
extern "C" JNIEXPORT void JNICALL
Java_com_hermes_agent_MainActivity_initNative(JNIEnv* env, jclass) {
    hermes_android_init(env);
}

// MainActivity 在后台线程调用,阻塞直到 server 结束(app 生命周期内持续运行)。
// server 启动逻辑与 Windows 的 main.cpp 一致(同一套 server 源码,仅入口不同)。
extern "C" JNIEXPORT void JNICALL
Java_com_hermes_agent_MainActivity_startServer(JNIEnv* env, jclass,
                                               jstring webui, jstring workspace,
                                               jstring data, jint port) {
    const char* wu = env->GetStringUTFChars(webui, nullptr);
    const char* ws = env->GetStringUTFChars(workspace, nullptr);
    const char* dt = env->GetStringUTFChars(data, nullptr);
    const std::string webui_s = wu ? wu : "";
    const std::string workspace_s = ws ? ws : "";
    const std::string data_s = dt ? dt : "";
    env->ReleaseStringUTFChars(webui, wu);
    env->ReleaseStringUTFChars(workspace, ws);
    env->ReleaseStringUTFChars(data, dt);

    try {
        hermes::AppContext ctx(webui_s, workspace_s, data_s);
        httplib::Server svr;
        // SSE 连接整个回合占一个池线程;池必须够大,否则 confirm/stop 请求会因无线程而死锁。
        svr.new_task_queue = [] { return new httplib::ThreadPool(32); };
        if (std::filesystem::exists(webui_s)) svr.set_base_dir(webui_s);
        hermes::register_routes(svr, ctx);
        svr.listen("127.0.0.1", static_cast<int>(port));
    } catch (...) {
        // server 启动异常静默;WebView 加载 localhost 失败时用户可见
    }
}
