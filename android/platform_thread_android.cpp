#include "platform_thread.hpp"
#include <jni.h>

extern "C" JavaVM* hermes_android_java_vm();

namespace hermes {

void platform_thread_enter() {
    JavaVM* vm = hermes_android_java_vm();
    if (!vm) return;
    JNIEnv* e = nullptr;
    if (vm->GetEnv(reinterpret_cast<void**>(&e), JNI_VERSION_1_6) != JNI_OK)
        vm->AttachCurrentThread(&e, nullptr);
}

void platform_thread_exit() {
    JavaVM* vm = hermes_android_java_vm();
    if (!vm) return;
    JNIEnv* e = nullptr;
    if (vm->GetEnv(reinterpret_cast<void**>(&e), JNI_VERSION_1_6) == JNI_OK)
        vm->DetachCurrentThread();
}

} // namespace hermes
