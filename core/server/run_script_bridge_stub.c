#include "run_script_bridge.h"
#include <stdlib.h>
#include <string.h>

/* Windows 桌面降级实现:MSVC 的 C 编译器无法编译 QuickJS(NaN-boxing 的 JSValue
   cast 存在系统性兼容问题)。脚本沙盒在 Android 上完整可用(NDK clang 编译)。 */
int hermes_script_run(const char* root, const char* script, char** out, int* is_error) {
    static const char msg[] =
        "[提示] 脚本沙盒(QuickJS)在 Android 上完整可用;Windows 桌面暂未内置该引擎,"
        "请改用 read_file / glob / grep / web_fetch 等工具完成该任务。";
    *out = (char*)malloc(sizeof(msg));
    if (*out) memcpy(*out, msg, sizeof(msg));
    *is_error = 1;
    return 1;
}
