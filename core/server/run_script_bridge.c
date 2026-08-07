/* 受限 JS 沙盒(QuickJS)。纯 C,避免 quickjs.h 的 C99 语法进入 C++ 编译单元。
   沙盒只读:注入 readFile/listDir/glob(限定工作目录内)+ console.log/error。
   护栏:内存 64MB + 5s 超时中断。不暴露写/网络/模块/进程。 */
#include "run_script_bridge.h"
#include "quickjs.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#include <io.h>
#define DIR_SEP '\\'
#else
#include <dirent.h>
#include <sys/stat.h>
#define DIR_SEP '/'
#endif

#define MAX_FILE_BYTES (1u << 20)
#define MEM_LIMIT (64ull << 20)
#define TIMEOUT_SEC 5

struct sandbox_state {
    const char* root;
    size_t root_len;
    char* output;
    size_t out_len, out_cap;
    unsigned deadline;   /* time(NULL) 绝对时刻,0 表示未设 */
};

static void st_append(struct sandbox_state* st, const char* s, size_t n) {
    if (st->out_len + n + 1 > st->out_cap) {
        size_t nc = st->out_cap ? st->out_cap * 2 : 256;
        while (nc < st->out_len + n + 1) nc *= 2;
        char* np = (char*)realloc(st->output, nc);
        if (!np) return;
        st->output = np;
        st->out_cap = nc;
    }
    memcpy(st->output + st->out_len, s, n);
    st->out_len += n;
    st->output[st->out_len] = 0;
}

/* 保守路径守卫:拒绝绝对路径与任何含 ".." 的相对路径(防逃逸) */
static int path_safe(const char* rel) {
    if (!rel || !*rel) return 0;
    if (rel[0] == '/' || rel[0] == '\\') return 0;
    if (strstr(rel, "..")) return 0;
    return 1;
}

static char* join_path(const struct sandbox_state* st, const char* rel) {
    size_t n = strlen(rel);
    char* p = (char*)malloc(st->root_len + n + 2);
    if (!p) return NULL;
    memcpy(p, st->root, st->root_len);
    size_t k = st->root_len;
    if (k && p[k - 1] != '/' && p[k - 1] != '\\') p[k++] = DIR_SEP;
    memcpy(p + k, rel, n);
    p[k + n] = 0;
    return p;
}

static char* read_file_text(const char* path, size_t* len) {
    FILE* f = fopen(path, "rb");
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
    long sz = ftell(f);
    if (sz < 0 || (size_t)sz > MAX_FILE_BYTES) { fclose(f); return NULL; }
    if (fseek(f, 0, SEEK_SET) != 0) { fclose(f); return NULL; }
    char* buf = (char*)malloc((size_t)sz + 1);
    if (!buf) { fclose(f); return NULL; }
    size_t rd = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    buf[rd] = 0;
    *len = rd;
    return buf;
}

static int interrupt_handler(JSRuntime* rt, void* opaque) {
    struct sandbox_state* st = (struct sandbox_state*)opaque;
    return st->deadline != 0 && (unsigned)time(NULL) > st->deadline;
}

/* ---- JS 注入函数 ---- */

static JSValue js_console(JSContext* ctx, JSValueConst, int argc, JSValueConst argv[]) {
    struct sandbox_state* st = (struct sandbox_state*)JS_GetContextOpaque(ctx);
    for (int i = 0; i < argc; ++i) {
        const char* s = JS_ToCString(ctx, argv[i]);
        if (s) { st_append(st, s, strlen(s)); JS_FreeCString(ctx, s); }
        if (i < argc - 1) st_append(st, " ", 1);
    }
    st_append(st, "\n", 1);
    return JS_UNDEFINED;
}

static JSValue js_read_file(JSContext* ctx, JSValueConst, int argc, JSValueConst argv[]) {
    struct sandbox_state* st = (struct sandbox_state*)JS_GetContextOpaque(ctx);
    if (argc < 1 || !JS_IsString(argv[0]))
        return JS_ThrowTypeError(ctx, "readFile 需要一个路径字符串");
    const char* rel = JS_ToCString(ctx, argv[0]);
    if (!rel) return JS_EXCEPTION;
    if (!path_safe(rel)) {
        JS_FreeCString(ctx, rel);
        return JS_ThrowTypeError(ctx, "路径越界");
    }
    char* full = join_path(st, rel);
    JS_FreeCString(ctx, rel);
    if (!full) return JS_EXCEPTION;
    size_t len = 0;
    char* text = read_file_text(full, &len);
    free(full);
    if (!text) return JS_ThrowTypeError(ctx, "无法读取文件");
    JSValue v = JS_NewStringLen(ctx, text, (int)len);
    free(text);
    return v;
}

static JSValue js_list_dir(JSContext* ctx, JSValueConst, int argc, JSValueConst argv[]) {
    struct sandbox_state* st = (struct sandbox_state*)JS_GetContextOpaque(ctx);
    const char* rel = ".";
    int rel_owned = 0;
    if (argc >= 1 && JS_IsString(argv[0])) {
        rel = JS_ToCString(ctx, argv[0]);
        if (!rel) return JS_EXCEPTION;
        rel_owned = 1;
        if (!path_safe(rel)) {
            JS_FreeCString(ctx, rel);
            return JS_ThrowTypeError(ctx, "路径越界");
        }
    }
    char* full = join_path(st, rel);
    if (rel_owned) JS_FreeCString(ctx, rel);
    if (!full) return JS_EXCEPTION;

    JSValue arr = JS_NewArray(ctx);
    uint32_t idx = 0;
#ifdef _WIN32
    {
        size_t n = strlen(full);
        char* pat = (char*)malloc(n + 3);
        if (!pat) { free(full); return JS_EXCEPTION; }
        memcpy(pat, full, n);
        pat[n] = DIR_SEP; pat[n + 1] = '*'; pat[n + 2] = 0;
        struct _finddata_t fd;
        intptr_t h = _findfirst(pat, &fd);
        free(pat);
        if (h != -1) {
            do {
                if (strcmp(fd.name, ".") == 0 || strcmp(fd.name, "..") == 0) continue;
                JSValue v = JS_NewString(ctx, fd.name);
                JS_SetPropertyUint32(ctx, arr, idx++, v);
            } while (_findnext(h, &fd) == 0);
            _findclose(h);
        }
    }
#else
    DIR* d = opendir(full);
    if (d) {
        struct dirent* e;
        while ((e = readdir(d))) {
            if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) continue;
            JSValue v = JS_NewString(ctx, e->d_name);
            JS_SetPropertyUint32(ctx, arr, idx++, v);
        }
        closedir(d);
    }
#endif
    free(full);
    return arr;
}

/* 简单 glob:支持 * 匹配任意(含 /)、? 单字符;不做 [] 字符类 */
static int glob_one(const char* pat, const char* txt) {
    if (!*pat) return !*txt;
    if (*pat == '*') {
        if (glob_one(pat + 1, txt)) return 1;
        return *txt && glob_one(pat, txt + 1);
    }
    if (*pat == '?') return *txt && glob_one(pat + 1, txt + 1);
    return *pat == *txt && glob_one(pat + 1, txt + 1);
}

/* 递归收集 base/rel_prefix 下的文件相对路径(跳过 denylist) */
static void collect_files(const char* base, const char* rel_prefix,
                          JSContext* ctx, JSValue arr, uint32_t* idx) {
    const size_t blen = strlen(base), rlen = strlen(rel_prefix);
    char* dir = (char*)malloc(blen + rlen + 2);
    if (!dir) return;
    memcpy(dir, base, blen);
    size_t k = blen;
    if (k && dir[k - 1] != '/' && dir[k - 1] != '\\') dir[k++] = DIR_SEP;
    memcpy(dir + k, rel_prefix, rlen);
    dir[k + rlen] = 0;

#ifdef _WIN32
    {
        const size_t n = strlen(dir);
        char* pat = (char*)malloc(n + 3);
        if (!pat) { free(dir); return; }
        memcpy(pat, dir, n); pat[n] = DIR_SEP; pat[n + 1] = '*'; pat[n + 2] = 0;
        struct _finddata_t fd;
        intptr_t h = _findfirst(pat, &fd);
        free(pat);
        if (h != -1) {
            do {
                if (strcmp(fd.name, ".") == 0 || strcmp(fd.name, "..") == 0) continue;
                char* sub = (char*)malloc(rlen + strlen(fd.name) + 2);
                if (!sub) continue;
                sub[0] = 0;
                if (rlen) { memcpy(sub, rel_prefix, rlen); sub[rlen] = '/'; strcpy(sub + rlen + 1, fd.name); }
                else strcpy(sub, fd.name);
                if (fd.attrib & _A_SUBDIR) {
                    if (strcmp(fd.name, "data") && strcmp(fd.name, ".git") &&
                        strcmp(fd.name, "build") && strcmp(fd.name, "node_modules"))
                        collect_files(base, sub, ctx, arr, idx);
                } else {
                    JSValue v = JS_NewString(ctx, sub);
                    JS_SetPropertyUint32(ctx, arr, (*idx)++, v);
                }
                free(sub);
            } while (_findnext(h, &fd) == 0);
            _findclose(h);
        }
    }
#else
    DIR* d = opendir(dir);
    if (d) {
        struct dirent* e;
        while ((e = readdir(d))) {
            if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) continue;
            char* sub = (char*)malloc(rlen + strlen(e->d_name) + 2);
            if (!sub) continue;
            sub[0] = 0;
            if (rlen) { memcpy(sub, rel_prefix, rlen); sub[rlen] = '/'; strcpy(sub + rlen + 1, e->d_name); }
            else strcpy(sub, e->d_name);
            char* full = (char*)malloc(strlen(dir) + strlen(e->d_name) + 2);
            if (!full) { free(sub); continue; }
            strcpy(full, dir);
            strcat(full, "/");
            strcat(full, e->d_name);
            struct stat stt;
            if (stat(full, &stt) == 0 && S_ISDIR(stt.st_mode)) {
                if (strcmp(e->d_name, "data") && strcmp(e->d_name, ".git") &&
                    strcmp(e->d_name, "build") && strcmp(e->d_name, "node_modules"))
                    collect_files(base, sub, ctx, arr, idx);
            } else {
                JSValue v = JS_NewString(ctx, sub);
                JS_SetPropertyUint32(ctx, arr, (*idx)++, v);
            }
            free(full);
            free(sub);
        }
        closedir(d);
    }
#endif
    free(dir);
}

static JSValue js_glob(JSContext* ctx, JSValueConst, int argc, JSValueConst argv[]) {
    struct sandbox_state* st = (struct sandbox_state*)JS_GetContextOpaque(ctx);
    if (argc < 1 || !JS_IsString(argv[0]))
        return JS_ThrowTypeError(ctx, "glob 需要一个模式字符串");
    const char* pat = JS_ToCString(ctx, argv[0]);
    if (!pat) return JS_EXCEPTION;

    JSValue arr = JS_NewArray(ctx);
    uint32_t idx = 0;
    /* 收集全部文件,再按模式过滤 */
    JSValue all = JS_NewArray(ctx);
    uint32_t all_n = 0;
    collect_files(st->root, "", ctx, all, &all_n);
    for (uint32_t i = 0; i < all_n; ++i) {
        JSValue v = JS_GetPropertyUint32(ctx, all, i);
        const char* f = JS_ToCString(ctx, v);
        if (f && glob_one(pat, f)) {
            JS_SetPropertyUint32(ctx, arr, idx++, JS_NewString(ctx, f));
        }
        if (f) JS_FreeCString(ctx, f);
        JS_FreeValue(ctx, v);
    }
    JS_FreeValue(ctx, all);
    JS_FreeCString(ctx, pat);
    return arr;
}

int hermes_script_run(const char* root, const char* script, char** out, int* is_error) {
    *out = NULL;
    *is_error = 0;
    if (!root || !script) return 1;

    JSRuntime* rt = JS_NewRuntime();
    if (!rt) return 1;
    JS_SetMemoryLimit(rt, MEM_LIMIT);
    struct sandbox_state st;
    memset(&st, 0, sizeof(st));
    st.root = root;
    st.root_len = strlen(root);
    st.deadline = (unsigned)time(NULL) + TIMEOUT_SEC;
    JS_SetInterruptHandler(rt, interrupt_handler, &st);

    JSContext* ctx = JS_NewContext(rt);
    if (!ctx) { JS_FreeRuntime(rt); return 1; }
    JS_SetContextOpaque(ctx, &st);

    /* 注入沙盒 API */
    JSValue global = JS_GetGlobalObject(ctx);
    JS_SetPropertyStr(ctx, global, "readFile", JS_NewCFunction(ctx, js_read_file, "readFile", 1));
    JS_SetPropertyStr(ctx, global, "listDir", JS_NewCFunction(ctx, js_list_dir, "listDir", 1));
    JS_SetPropertyStr(ctx, global, "glob", JS_NewCFunction(ctx, js_glob, "glob", 1));
    JSValue console = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, console, "log", JS_NewCFunction(ctx, js_console, "log", 1));
    JS_SetPropertyStr(ctx, console, "error", JS_NewCFunction(ctx, js_console, "error", 1));
    JS_SetPropertyStr(ctx, global, "console", console);
    JS_FreeValue(ctx, console);
    JS_FreeValue(ctx, global);

    JSValue ret = JS_Eval(ctx, script, strlen(script), "<script>", JS_EVAL_TYPE_GLOBAL);
    if (JS_IsException(ret)) {
        JSValue exc = JS_GetException(ctx);
        const char* msg = JS_ToCString(ctx, exc);
        st_append(&st, "[错误] ", 6);
        if (msg) { st_append(&st, msg, strlen(msg)); JS_FreeCString(ctx, msg); }
        JS_FreeValue(ctx, exc);
        *is_error = 1;
    } else {
        if (!JS_IsUndefined(ret) && !JS_IsNull(ret)) {
            const char* r = JS_ToCString(ctx, ret);
            if (r) {
                if (st.out_len && st.output[st.out_len - 1] != '\n') st_append(&st, "\n", 1);
                st_append(&st, "=> ", 3);
                st_append(&st, r, strlen(r));
                JS_FreeCString(ctx, r);
            }
        }
        JS_FreeValue(ctx, ret);
    }

    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);

    if (!st.output) st.output = (char*)malloc(1);
    if (!st.output) return 1;
    if (!st.out_len) { st.output[0] = 0; }
    *out = st.output;
    return *is_error ? 1 : 0;
}
