#include "http_transport_winhttp.hpp"
#include <jni.h>
#include <string>
#include <vector>

// Android 版出站 HTTP:通过 JNI 调 Java 的 HttpURLConnection(系统自带 TLS)。
// 关键:JNI 类/方法必须在 app 主线程初始化并缓存为全局引用——server 的 worker 线程
// (非 Java 线程)里 FindClass 用系统 classloader,找不到 app 的类,会返回 NULL。
// MainActivity.onCreate 调 initNative() 完成缓存。C++ 侧不再有平台 #ifdef。

namespace hermes {

static JavaVM* g_vm = nullptr;
static jclass g_http_class = nullptr;   // 全局引用,跨线程有效
static jmethodID g_open_m = nullptr, g_read_m = nullptr, g_close_m = nullptr,
                 g_status_m = nullptr, g_header_m = nullptr;

// 主线程调用:缓存 HermesHttp 类引用与全部方法 ID
extern "C" JNIEXPORT void JNICALL hermes_android_init(JNIEnv* env) {
    if (env->GetJavaVM(&g_vm) != JNI_OK) return;
    jclass local = env->FindClass("com/hermes/agent/HermesHttp");
    if (!local) { env->ExceptionClear(); return; }
    g_http_class = static_cast<jclass>(env->NewGlobalRef(local));
    env->DeleteLocalRef(local);
    g_open_m = env->GetStaticMethodID(
        g_http_class, "open",
        "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;[BII)Lcom/hermes/agent/HermesHttp$HttpStream;");
    g_read_m = env->GetStaticMethodID(g_http_class, "readChunk",
        "(Lcom/hermes/agent/HermesHttp$HttpStream;[B)I");
    g_close_m = env->GetStaticMethodID(g_http_class, "close",
        "(Lcom/hermes/agent/HermesHttp$HttpStream;)V");
    g_status_m = env->GetStaticMethodID(g_http_class, "status",
        "(Lcom/hermes/agent/HermesHttp$HttpStream;)I");
    g_header_m = env->GetStaticMethodID(g_http_class, "getHeader",
        "(Lcom/hermes/agent/HermesHttp$HttpStream;Ljava/lang/String;)Ljava/lang/String;");
    env->ExceptionClear();
}

extern "C" JavaVM* hermes_android_java_vm() { return g_vm; }

namespace {

JNIEnv* jenv() {
    if (!g_vm) return nullptr;
    JNIEnv* e = nullptr;
    if (g_vm->GetEnv(reinterpret_cast<void**>(&e), JNI_VERSION_1_6) != JNI_OK)
        g_vm->AttachCurrentThread(&e, nullptr);
    return e;
}

// "k1v2k2v2" → string
std::string encode_headers(const std::vector<std::pair<std::string, std::string>>& hdrs) {
    std::string out;
    for (const auto& kv : hdrs) {
        if (!out.empty()) out += '\x02';
        out += kv.first + '\x01' + kv.second;
    }
    return out;
}

} // namespace

HttpTransport make_http_transport(const std::atomic<bool>& abort, unsigned timeout_ms,
                                  unsigned receive_idle_ms) {
    if (receive_idle_ms == 0) receive_idle_ms = timeout_ms;
    return [&abort, timeout_ms, receive_idle_ms](const HttpRequest& hr,
                                                 const BodyChunkCallback& cb) -> HttpResponse {
        HttpResponse out;
        JNIEnv* env = jenv();
        if (!env || !g_http_class || !g_open_m) {
            out.error = "JNI not initialized";
            return out;
        }

        jstring jmethod = env->NewStringUTF(hr.method.c_str());
        jstring jurl = env->NewStringUTF(hr.url.c_str());
        jstring jheaders = env->NewStringUTF(encode_headers(hr.headers).c_str());
        jbyteArray jbody = nullptr;
        if (!hr.body.empty()) {
            jbody = env->NewByteArray(static_cast<jsize>(hr.body.size()));
            env->SetByteArrayRegion(jbody, 0, static_cast<jsize>(hr.body.size()),
                                    reinterpret_cast<const jbyte*>(hr.body.data()));
        }

        jobject local = env->CallStaticObjectMethod(g_http_class, g_open_m, jmethod, jurl,
                                                    jheaders, jbody, (jint)timeout_ms,
                                                    (jint)receive_idle_ms);
        env->DeleteLocalRef(jmethod);
        env->DeleteLocalRef(jurl);
        env->DeleteLocalRef(jheaders);
        if (jbody) env->DeleteLocalRef(jbody);

        if (env->ExceptionCheck() || !local) {
            env->ExceptionClear();
            out.status = 0;
            out.stream_complete = false;
            out.error = "http open failed";
            return out;
        }
        jobject stream = env->NewGlobalRef(local);
        env->DeleteLocalRef(local);

        out.status = env->CallStaticIntMethod(g_http_class, g_status_m, stream);

        auto get_hdr = [&](const char* name) -> std::string {
            jstring jn = env->NewStringUTF(name);
            jstring jv = (jstring)env->CallStaticObjectMethod(g_http_class, g_header_m, stream, jn);
            env->DeleteLocalRef(jn);
            if (!jv) return "";
            const char* c = env->GetStringUTFChars(jv, nullptr);
            std::string s = c ? c : "";
            env->ReleaseStringUTFChars(jv, c);
            env->DeleteLocalRef(jv);
            return s;
        };
        out.content_type = get_hdr("Content-Type");
        out.mcp_session_id = get_hdr("Mcp-Session-Id");

        // 流式拉取:逐块回调,abort 时中断
        constexpr int kBufSize = 65536;
        for (;;) {
            if (abort.load()) {
                out.status = 0;
                out.error = "aborted";
                out.stream_complete = false;
                break;
            }
            jbyteArray buf = env->NewByteArray(kBufSize);
            jint n = env->CallStaticIntMethod(g_http_class, g_read_m, stream, buf);
            if (env->ExceptionCheck()) {
                env->ExceptionClear();
                out.stream_complete = false;
                out.error = "stream read error";
                env->DeleteLocalRef(buf);
                break;
            }
            if (n <= 0) {
                env->DeleteLocalRef(buf);
                break;  // EOF,流完整
            }
            jbyte* elems = env->GetByteArrayElements(buf, nullptr);
            cb(std::string(reinterpret_cast<const char*>(elems), n));
            env->ReleaseByteArrayElements(buf, elems, JNI_ABORT);
            env->DeleteLocalRef(buf);
        }

        env->CallStaticVoidMethod(g_http_class, g_close_m, stream);
        env->DeleteGlobalRef(stream);
        return out;
    };
}

} // namespace hermes
