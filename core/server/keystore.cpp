#include "keystore.hpp"
#include <filesystem>
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

void bind_text(sqlite3_stmt* s, int idx, const std::string& v) {
    sqlite3_bind_text(s, idx, v.c_str(), -1, SQLITE_TRANSIENT);
}

} // namespace

KeyStore::KeyStore(const std::filesystem::path& db_path) {
    std::error_code ec;
    std::filesystem::create_directories(db_path.parent_path(), ec);
    if (sqlite3_open(db_path.string().c_str(), &db_) != SQLITE_OK) {
        throw std::runtime_error("sqlite: 无法打开数据库 " + db_path.string());
    }
    exec(db_, "CREATE TABLE IF NOT EXISTS keys ("
              " provider_id TEXT PRIMARY KEY, key_value TEXT NOT NULL);");
}

KeyStore::~KeyStore() {
    if (db_) sqlite3_close(db_);
}

void KeyStore::set(const std::string& provider, const std::string& key) {
    std::lock_guard<std::mutex> lk(mtx_);
    if (key.empty()) {
        Stmt del(db_, "DELETE FROM keys WHERE provider_id=?");
        bind_text(del.s, 1, provider);
        sqlite3_step(del.s);
        return;
    }
    Stmt up(db_,
        "INSERT INTO keys(provider_id,key_value) VALUES(?,?) "
        "ON CONFLICT(provider_id) DO UPDATE SET key_value=excluded.key_value");
    bind_text(up.s, 1, provider);
    bind_text(up.s, 2, key);
    if (sqlite3_step(up.s) != SQLITE_DONE) die(db_, "upsert key: ");
}

std::optional<std::string> KeyStore::get(const std::string& provider) const {
    std::lock_guard<std::mutex> lk(mtx_);
    Stmt s(db_, "SELECT key_value FROM keys WHERE provider_id=?");
    bind_text(s.s, 1, provider);
    if (sqlite3_step(s.s) != SQLITE_ROW) return std::nullopt;
    const auto* v = reinterpret_cast<const char*>(sqlite3_column_text(s.s, 0));
    return v ? std::optional<std::string>(v) : std::nullopt;
}

bool KeyStore::has(const std::string& provider) const {
    return get(provider).has_value();
}

} // namespace hermes
