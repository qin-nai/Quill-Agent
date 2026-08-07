#ifndef HERMES_RUN_SCRIPT_BRIDGE_H
#define HERMES_RUN_SCRIPT_BRIDGE_H

#ifdef __cplusplus
extern "C" {
#endif

/* 在受限 JS 沙盒(QuickJS)中执行脚本。
   返回 0 成功;1 脚本出错/异常(此时 *out 含错误信息)。
   *out 为 malloc 的结果字符串,调用方必须 free。
   root 为工作目录,脚本只能通过 readFile/listDir/glob 访问其内部(防逃逸)。
   内存上限 64MB,超时 5s。 */
int hermes_script_run(const char* root, const char* script, char** out, int* is_error);

#ifdef __cplusplus
}
#endif

#endif /* HERMES_RUN_SCRIPT_BRIDGE_H */
