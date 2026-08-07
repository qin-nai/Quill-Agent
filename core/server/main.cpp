#include "app_context.hpp"
#include "routes.hpp"
#include <httplib.h>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <stdexcept>

int main(int argc, char* argv[]) {
    using namespace hermes;

    std::filesystem::path webui, workspace, data;
    int port = 8090;

    auto take = [&](int& i, const char* flag) -> std::string {
        if (i + 1 >= argc) {
            std::cerr << flag << " 缺少参数\n";
            std::exit(2);
        }
        return argv[++i];
    };
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--webui") webui = take(i, "--webui");
        else if (a == "--workspace") workspace = take(i, "--workspace");
        else if (a == "--data") data = take(i, "--data");
        else if (a == "--port") port = std::atoi(take(i, "--port").c_str());
        else {
            std::cerr << "未知参数: " << a << "\n";
            return 2;
        }
    }

    std::error_code ec;
    if (webui.empty()) webui = std::filesystem::current_path(ec) / "webui";
    if (workspace.empty()) workspace = std::filesystem::current_path(ec) / "workspace";
    if (data.empty()) data = workspace / "data";

    try {
        AppContext ctx(webui, workspace, data);

        httplib::Server svr;
        // SSE 连接整个回合占一个池线程;池必须够大,否则 confirm/stop 请求会因无线程而死锁。
        svr.new_task_queue = [] { return new httplib::ThreadPool(32); };
        if (std::filesystem::exists(webui)) svr.set_base_dir(webui.string());
        register_routes(svr, ctx);

        std::cout << "Quill Agent server → http://127.0.0.1:" << port
                  << "  webui=" << webui
                  << "  workspace=" << workspace
                  << "  data=" << data << "\n";
        if (!svr.listen("127.0.0.1", port)) {
            std::cerr << "listen 失败(端口被占用?)\n";
            return 1;
        }
    } catch (const std::exception& e) {
        std::cerr << "启动失败: " << e.what() << "\n";
        return 1;
    }
    return 0;
}
