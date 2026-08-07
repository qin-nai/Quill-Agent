#include "session_store.hpp"
#include "hermes/json_io.hpp"
#include <nlohmann/json.hpp>
#include <chrono>
#include <ctime>
#include <random>
#include <sstream>
#include <stdexcept>

namespace hermes {

namespace {

[[noreturn]] void die(sqlite3* db, const std::string& what) {
    throw std::runtime_error("sqlite: " + what + (db ? std::string(sqlite3_errmsg(db)) : ""));
}

struct Stmt {
    sqlite3_stmt* s = nullptr;
    explicit Stmt(sqlite3* db, const char* sql) {
        if (sqlite3_prepare_v2(db, sql, -1, &s, nullptr) != SQLITE_OK) die(db, "prepare: ");
    }
    ~Stmt() { if (s) sqlite3_finalize(s); }
    Stmt(const Stmt&) = delete;
    Stmt& operator=(const Stmt&) = delete;
};

void exec(sqlite3* db, const char* sql) {
    char* err = nullptr;
    if (sqlite3_exec(db, sql, nullptr, nullptr, &err) != SQLITE_OK) {
        std::string msg = err ? err : "exec failed";
        sqlite3_free(err);
        throw std::runtime_error("sqlite: " + msg);
    }
}

std::string now_iso() {
    const auto t = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    std::tm tm{};
#ifdef _MSC_VER
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    std::ostringstream os;
    os << std::put_time(&tm, "%Y-%m-%dT%H:%M:%S");
    return os.str();
}

std::string new_id() {
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    static std::mt19937 rng{std::random_device{}()};
    return "s_" + std::to_string(ms) + "_" + std::to_string(rng() % 100000);
}

// 取前 max_bytes 字节,不在 UTF-8 多字节序列中间截断(避免中文乱码)。
std::string truncate_utf8(const std::string& s, size_t max_bytes) {
    if (s.size() <= max_bytes) return s;
    size_t end = max_bytes;
    while (end > 0 && (static_cast<unsigned char>(s[end]) & 0xC0) == 0x80) --end;
    return s.substr(0, end) + "…";
}

// 用首条用户消息生成会话标题:折叠空白 + 截断到约 40 字节。
std::string make_title(const Message& msg) {
    std::string flat;
    bool prev_space = false;
    for (const auto& b : msg.content) {
        if (b.type != BlockType::Text) continue;
        for (char c : b.text) {
            if (c == '\n' || c == '\r' || c == '\t' || c == ' ') {
                if (!prev_space && !flat.empty()) flat += ' ';
                prev_space = true;
            } else {
                flat += c;
                prev_space = false;
            }
        }
    }
    if (flat.empty()) return {};
    return truncate_utf8(flat, 40);
}

Session row_to_session(sqlite3_stmt* s) {
    Session out;
    out.id = reinterpret_cast<const char*>(sqlite3_column_text(s, 0));
    out.title = reinterpret_cast<const char*>(sqlite3_column_text(s, 1));
    out.provider = reinterpret_cast<const char*>(sqlite3_column_text(s, 2));
    out.model = reinterpret_cast<const char*>(sqlite3_column_text(s, 3));
    out.created_at = reinterpret_cast<const char*>(sqlite3_column_text(s, 4));
    const std::string msgs = reinterpret_cast<const char*>(sqlite3_column_text(s, 5));
    out.messages = nlohmann::json::parse(msgs).get<std::vector<Message>>();
    return out;
}

void bind_text(sqlite3_stmt* s, int idx, const std::string& v) {
    sqlite3_bind_text(s, idx, v.c_str(), -1, SQLITE_TRANSIENT);
}

} // namespace

SessionStore::SessionStore(const std::filesystem::path& db_path) {
    std::error_code ec;
    std::filesystem::create_directories(db_path.parent_path(), ec);
    if (sqlite3_open(db_path.string().c_str(), &db_) != SQLITE_OK) {
        throw std::runtime_error("sqlite: 无法打开数据库 " + db_path.string());
    }
    exec(db_, "CREATE TABLE IF NOT EXISTS sessions ("
              " id TEXT PRIMARY KEY, title TEXT NOT NULL, provider TEXT NOT NULL,"
              " model TEXT NOT NULL, created_at TEXT NOT NULL, messages TEXT NOT NULL);");
}

SessionStore::~SessionStore() {
    if (db_) sqlite3_close(db_);
}

std::vector<SessionMeta> SessionStore::list() const {
    std::lock_guard<std::mutex> lk(mtx_);
    std::vector<SessionMeta> out;
    Stmt s(db_, "SELECT id,title,provider,model,created_at FROM sessions ORDER BY created_at DESC");
    while (sqlite3_step(s.s) == SQLITE_ROW) {
        SessionMeta m;
        m.id = reinterpret_cast<const char*>(sqlite3_column_text(s.s, 0));
        m.title = reinterpret_cast<const char*>(sqlite3_column_text(s.s, 1));
        m.provider = reinterpret_cast<const char*>(sqlite3_column_text(s.s, 2));
        m.model = reinterpret_cast<const char*>(sqlite3_column_text(s.s, 3));
        m.created_at = reinterpret_cast<const char*>(sqlite3_column_text(s.s, 4));
        out.push_back(std::move(m));
    }
    return out;
}

std::optional<Session> SessionStore::get_unlocked(const std::string& id) const {
    Stmt s(db_, "SELECT id,title,provider,model,created_at,messages FROM sessions WHERE id=?");
    bind_text(s.s, 1, id);
    if (sqlite3_step(s.s) != SQLITE_ROW) return std::nullopt;
    return row_to_session(s.s);
}

std::optional<Session> SessionStore::get(const std::string& id) const {
    std::lock_guard<std::mutex> lk(mtx_);
    return get_unlocked(id);
}

Session SessionStore::create(const std::string& provider, const std::string& model,
                             const std::string& title) {
    std::lock_guard<std::mutex> lk(mtx_);
    Session out;
    out.id = new_id();
    out.title = title.empty() ? "新会话" : title;
    out.provider = provider;
    out.model = model;
    out.created_at = now_iso();
    const std::string msgs = nlohmann::json::array().dump();

    Stmt ins(db_, "INSERT INTO sessions(id,title,provider,model,created_at,messages) VALUES(?,?,?,?,?,?)");
    bind_text(ins.s, 1, out.id);
    bind_text(ins.s, 2, out.title);
    bind_text(ins.s, 3, out.provider);
    bind_text(ins.s, 4, out.model);
    bind_text(ins.s, 5, out.created_at);
    bind_text(ins.s, 6, msgs);
    if (sqlite3_step(ins.s) != SQLITE_DONE) die(db_, "insert session: ");
    return out;
}

void SessionStore::replace_unlocked(const std::string& id, const std::vector<Message>& msgs) {
    const std::string msgs_json = nlohmann::json(msgs).dump();
    Stmt up(db_, "UPDATE sessions SET messages=? WHERE id=?");
    bind_text(up.s, 1, msgs_json);
    bind_text(up.s, 2, id);
    sqlite3_step(up.s);
}

void SessionStore::append_message(const std::string& id, const Message& msg) {
    std::lock_guard<std::mutex> lk(mtx_);
    auto cur = get_unlocked(id);
    if (!cur) return;
    // 仅默认标题("新会话")的会话,在首条用户消息时自动命名;显式标题不覆盖
    const bool auto_title = cur->messages.empty() && msg.role == "user" && cur->title == "新会话";
    cur->messages.push_back(msg);
    replace_unlocked(id, cur->messages);
    if (auto_title) {
        const std::string title = make_title(msg);
        if (!title.empty()) {
            Stmt up(db_, "UPDATE sessions SET title=? WHERE id=?");
            bind_text(up.s, 1, title);
            bind_text(up.s, 2, id);
            sqlite3_step(up.s);
        }
    }
}

void SessionStore::replace_messages(const std::string& id, const std::vector<Message>& msgs) {
    std::lock_guard<std::mutex> lk(mtx_);
    replace_unlocked(id, msgs);
}

void SessionStore::update_model(const std::string& id, const std::string& model) {
    std::lock_guard<std::mutex> lk(mtx_);
    Stmt up(db_, "UPDATE sessions SET model=? WHERE id=?");
    bind_text(up.s, 1, model);
    bind_text(up.s, 2, id);
    sqlite3_step(up.s);
}

} // namespace hermes
