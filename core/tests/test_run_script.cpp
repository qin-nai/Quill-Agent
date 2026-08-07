#include "hermes/tools/run_script_tool.hpp"
#include <filesystem>
#include <fstream>
#include <iostream>

using namespace hermes;

static int g_fail = 0;
#define CHECK(cond) do { if (!(cond)) { ++g_fail; std::cerr << "FAIL " << __LINE__ << ": " #cond "\n"; } } while (0)

int main() {
    const auto dir = std::filesystem::temp_directory_path() / "hermes_run_script_test";
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir);
    { std::ofstream f(dir / "demo.txt"); f << "hello sandbox\n"; }

    RunScriptTool t(dir);

    // 1) 算术
    const auto r1 = t.execute({{"script", "1 + 2"}});
    CHECK(!r1.is_error && r1.output.find("3") != std::string::npos);

    // 2) console.log + 表达式结果
    const auto r2 = t.execute({{"script", "console.log('hi'); 40 + 2"}});
    CHECK(!r2.is_error && r2.output.find("hi") != std::string::npos
          && r2.output.find("42") != std::string::npos);

    // 3) readFile 工作目录内
    const auto r3 = t.execute({{"script", "readFile('demo.txt')"}});
    CHECK(!r3.is_error && r3.output.find("hello sandbox") != std::string::npos);

    // 4) 越界拒绝
    const auto r4 = t.execute({{"script", "readFile('../outside.txt')"}});
    CHECK(r4.is_error && r4.output.find("越界") != std::string::npos);

    // 5) 语法错误
    const auto r5 = t.execute({{"script", "function {"}});
    CHECK(r5.is_error);

    // 6) 死循环 5s 中断
    const auto r6 = t.execute({{"script", "while(true){}"}});
    CHECK(r6.is_error);

    // 7) glob
    const auto r7 = t.execute({{"script", "glob('*.txt')"}});
    CHECK(!r7.is_error && r7.output.find("demo.txt") != std::string::npos);

    // 8) listDir
    const auto r8 = t.execute({{"script", "listDir('.')"}});
    CHECK(!r8.is_error && r8.output.find("demo.txt") != std::string::npos);

    std::filesystem::remove_all(dir, ec);
    if (g_fail) { std::cerr << g_fail << " 项失败\n"; return 1; }
    std::cout << "run_script 测试通过\n";
    return 0;
}
