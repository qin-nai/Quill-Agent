#include "session_store.hpp"
#include <iostream>
#include <filesystem>

using namespace hermes;

static int g_fail = 0;
#define CHECK(cond) do { if (!(cond)) { ++g_fail; std::cerr << "FAIL " << __LINE__ << ": " #cond "\n"; } } while (0)

int main() {
    const auto db = std::filesystem::temp_directory_path() / "hermes_test_sessions.db";
    std::error_code ec;
    std::filesystem::remove(db, ec);
    {
        SessionStore store(db);

        // 1) 空标题创建 → 固定"新会话"
        Session s = store.create("deepseek", "deepseek-v4-flash", "");
        CHECK(s.title == "新会话");

        // 2) 首条用户消息(短)→ 用消息开头命名,且不超过 40 字节不截断
        Message m;
        m.role = "user";
        m.content.push_back(ContentBlock::make_text("请修复登录页面的 bug"));
        store.append_message(s.id, m);
        auto s2 = store.get(s.id);
        CHECK(s2.has_value());
        CHECK(s2->title == "请修复登录页面的 bug");

        // 3) 第二条消息 → 标题保持不变
        Message m2;
        m2.role = "user";
        m2.content.push_back(ContentBlock::make_text("第二条消息,不应改标题"));
        store.append_message(s.id, m2);
        auto s3 = store.get(s.id);
        CHECK(s3->title == s2->title);

        // 4) 超长消息 → 截断,末尾省略号,且不以半个 UTF-8 字符结尾(无乱码)
        Session s4 = store.create("deepseek", "deepseek-v4-flash", "");
        Message m4;
        m4.role = "user";
        m4.content.push_back(ContentBlock::make_text(
            "这是一个非常非常长的标题,用来测试截断功能是否正确工作,超过四十字节之后就应该出现省略号并保持可读"));
        store.append_message(s4.id, m4);
        auto s5 = store.get(s4.id);
        CHECK(s5->title.size() > 0);
        CHECK(s5->title.size() >= 3 && s5->title.substr(s5->title.size() - 3) == "…");  // UTF-8 省略号
        CHECK(s5->title.size() <= 40 + 3);  // 40 字节内容 + 省略号(3 字节 UTF-8)

        // 5) 显式标题创建 → 首条消息不改名
        Session s6 = store.create("deepseek", "deepseek-v4-flash", "手动标题");
        store.append_message(s6.id, m);
        auto s7 = store.get(s6.id);
        CHECK(s7->title == "手动标题");

        // 6) update_model 持久化(模型栏切换生效)
        store.update_model(s.id, "deepseek-v4-pro");
        auto s8 = store.get(s.id);
        CHECK(s8->model == "deepseek-v4-pro");
    }
    std::filesystem::remove(db, ec);
    if (g_fail) { std::cerr << g_fail << " 项失败\n"; return 1; }
    std::cout << "session_store 测试通过\n";
    return 0;
}
