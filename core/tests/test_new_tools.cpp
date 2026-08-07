#include "hermes/skills.hpp"
#include "hermes/tools/glob.hpp"
#include "hermes/tools/glob_tool.hpp"
#include "hermes/tools/grep_tool.hpp"
#include "hermes/tools/todo_tool.hpp"
#include "hermes/tools/web_utils.hpp"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

using namespace hermes;
namespace fs = std::filesystem;

static int g_fail = 0;
#define CHECK(cond) do { if (!(cond)) { ++g_fail; std::cerr << "FAIL " << __LINE__ << ": " #cond "\n"; } } while (0)

static void test_glob() {
    CHECK(glob_match("*.cpp", "a.cpp"));
    CHECK(glob_match("**/*.cpp", "src/a.cpp"));
    CHECK(glob_match("src/*.cpp", "src/a.cpp"));
    CHECK(!glob_match("src/*.cpp", "src/sub/a.cpp"));
    CHECK(glob_match("file[0-9].txt", "file5.txt"));
    CHECK(!glob_match("file[0-9].txt", "filex.txt"));
    CHECK(glob_match("a?.cpp", "ab.cpp"));
    CHECK(!glob_match("a?.cpp", "a/b.cpp"));
    CHECK(!glob_match("*.cpp", "a.cpp.h"));
}

static void test_html() {
    CHECK(url_encode("a b&c") == "a+b%26c");
    CHECK(url_decode("a+b%26c") == "a b&c");
    const std::string h =
        "<html><head><script>var x=1;</script><style>p{}</style></head><body>"
        "<h1>标题</h1><p>一段<b>加粗</b> &amp; 实体</p>"
        "<ul><li>项1</li><li>项2</li></ul></body></html>";
    const std::string t = html_to_text(h);
    CHECK(t.find("标题") != std::string::npos);
    CHECK(t.find("一段") != std::string::npos);
    CHECK(t.find("加粗") != std::string::npos);
    CHECK(t.find("&amp;") == std::string::npos);
    CHECK(t.find("var x=1") == std::string::npos);  // script 剔除
    CHECK(t.find("p{}") == std::string::npos);      // style 剔除
    CHECK(t.find("项1") != std::string::npos);
}

static void test_skills(const fs::path& dir) {
    fs::create_directories(dir / ".hermes" / "skills" / "demo");
    { std::ofstream f(dir / ".hermes" / "skills" / "demo" / "SKILL.md");
      f << "---\nname: demo\ndescription: 演示技能\n---\n\n# 正文\n用法...\n"; }
    { std::ofstream f(dir / ".hermes" / "skills" / "demo" / "helper.py"); f << "x=1\n"; }
    fs::create_directories(dir / "user_skills" / "other");
    { std::ofstream f(dir / "user_skills" / "other" / "SKILL.md");
      f << "---\nname: other\ndescription: 另一个\n---\n"; }
    fs::create_directories(dir / ".hermes" / "skills" / "bad");
    { std::ofstream f(dir / ".hermes" / "skills" / "bad" / "SKILL.md"); f << "没有 frontmatter\n"; }

    const auto skills = scan_skills(dir, dir / "user_skills");
    CHECK(skills.size() == 2);
    bool has_demo = false, has_other = false;
    for (const auto& s : skills) {
        if (s.name == "demo") { has_demo = true; CHECK(s.helper_files.size() == 1); }
        if (s.name == "other") has_other = true;
    }
    CHECK(has_demo && has_other);
}

static void test_todo() {
    TodoTool t;
    const auto r1 = t.execute({{"action", "add"}, {"text", "写文档"}});
    CHECK(!r1.is_error && r1.output.find("写文档") != std::string::npos);
    const auto r2 = t.execute({{"action", "add"}, {"text", "跑测试"}});
    CHECK(!r2.is_error);
    const auto r3 = t.execute({{"action", "list"}});
    CHECK(r3.output.find("写文档") != std::string::npos && r3.output.find("跑测试") != std::string::npos);
    const auto r4 = t.execute({{"action", "update"}, {"id", "1"}, {"status", "completed"}});
    CHECK(!r4.is_error);
    const auto r5 = t.execute({{"action", "list"}});
    CHECK(r5.output.find("completed") != std::string::npos);
    const auto r6 = t.execute({{"action", "clear"}});
    CHECK(!r6.is_error);
    const auto r7 = t.execute({{"action", "list"}});
    CHECK(r7.output.find("暂无") != std::string::npos);
}

static void test_glob_grep_tools(const fs::path& dir) {
    fs::create_directories(dir / "src");
    { std::ofstream f(dir / "src" / "main.cpp"); f << "int main(){ return 0; }\n"; }
    { std::ofstream f(dir / "src" / "util.h"); f << "#pragma once\nint helper();\n"; }
    { std::ofstream f(dir / "README.md"); f << "docs\n"; }

    GlobTool g(dir);
    const auto r1 = g.execute({{"pattern", "**/*.cpp"}});
    CHECK(!r1.is_error && r1.output.find("src/main.cpp") != std::string::npos);
    const auto r2 = g.execute({{"pattern", "**/*.md"}});
    CHECK(r2.output.find("README.md") != std::string::npos);

    GrepTool gt(dir);
    const auto gr1 = gt.execute({{"pattern", "int main"}});
    CHECK(!gr1.is_error && gr1.output.find("src/main.cpp:1") != std::string::npos);
    const auto gr2 = gt.execute({{"pattern", "HELPER"}});  // 默认忽略大小写
    CHECK(gr2.output.find("util.h:2") != std::string::npos);
}

int main() {
    const fs::path dir = fs::temp_directory_path() / "hermes_new_tools_test";
    fs::remove_all(dir);
    fs::create_directories(dir);

    test_glob();
    test_html();
    test_skills(dir);
    test_todo();
    test_glob_grep_tools(dir);

    fs::remove_all(dir);
    if (g_fail) {
        std::cerr << g_fail << " 项失败\n";
        return 1;
    }
    std::cout << "new tools 测试通过\n";
    return 0;
}
