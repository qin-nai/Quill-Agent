#include "model_store.hpp"
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

ModelStore::ModelStore(const std::filesystem::path& db_path) {
    std::error_code ec;
    std::filesystem::create_directories(db_path.parent_path(), ec);
    if (sqlite3_open(db_path.string().c_str(), &db_) != SQLITE_OK) {
        throw std::runtime_error("sqlite: 无法打开数据库 " + db_path.string());
    }
    exec(db_, "CREATE TABLE IF NOT EXISTS custom_models ("
              " provider_id TEXT NOT NULL, model TEXT NOT NULL,"
              " PRIMARY KEY(provider_id, model));");
    exec(db_, "CREATE TABLE IF NOT EXISTS removed_builtin ("
              " provider_id TEXT NOT NULL, model TEXT NOT NULL,"
              " PRIMARY KEY(provider_id, model));");
    exec(db_, "CREATE TABLE IF NOT EXISTS model_ctx ("
              " provider_id TEXT NOT NULL, model TEXT NOT NULL,"
              " context_window INTEGER NOT NULL,"
              " PRIMARY KEY(provider_id, model));");
}

ModelStore::~ModelStore() {
    if (db_) sqlite3_close(db_);
}

std::vector<std::string> ModelStore::list(const std::string& provider) const {
    std::lock_guard<std::mutex> lk(mtx_);
    std::vector<std::string> out;
    Stmt s(db_, "SELECT model FROM custom_models WHERE provider_id=? ORDER BY model");
    bind_text(s.s, 1, provider);
    while (sqlite3_step(s.s) == SQLITE_ROW)
        out.emplace_back(reinterpret_cast<const char*>(sqlite3_column_text(s.s, 0)));
    return out;
}

void ModelStore::add(const std::string& provider, const std::string& model) {
    std::lock_guard<std::mutex> lk(mtx_);
    Stmt ins(db_, "INSERT OR IGNORE INTO custom_models(provider_id,model) VALUES(?,?)");
    bind_text(ins.s, 1, provider);
    bind_text(ins.s, 2, model);
    if (sqlite3_step(ins.s) != SQLITE_DONE) die(db_, "insert custom model: ");
}

void ModelStore::remove(const std::string& provider, const std::string& model) {
    std::lock_guard<std::mutex> lk(mtx_);
    Stmt del(db_, "DELETE FROM custom_models WHERE provider_id=? AND model=?");
    bind_text(del.s, 1, provider);
    bind_text(del.s, 2, model);
    sqlite3_step(del.s);
}

std::vector<std::string> ModelStore::list_removed(const std::string& provider) const {
    std::lock_guard<std::mutex> lk(mtx_);
    std::vector<std::string> out;
    Stmt s(db_, "SELECT model FROM removed_builtin WHERE provider_id=? ORDER BY model");
    bind_text(s.s, 1, provider);
    while (sqlite3_step(s.s) == SQLITE_ROW)
        out.emplace_back(reinterpret_cast<const char*>(sqlite3_column_text(s.s, 0)));
    return out;
}

void ModelStore::add_removed(const std::string& provider, const std::string& model) {
    std::lock_guard<std::mutex> lk(mtx_);
    Stmt ins(db_, "INSERT OR IGNORE INTO removed_builtin(provider_id,model) VALUES(?,?)");
    bind_text(ins.s, 1, provider);
    bind_text(ins.s, 2, model);
    if (sqlite3_step(ins.s) != SQLITE_DONE) die(db_, "insert removed builtin: ");
}

void ModelStore::clear_removed(const std::string& provider, const std::string& model) {
    std::lock_guard<std::mutex> lk(mtx_);
    Stmt del(db_, "DELETE FROM removed_builtin WHERE provider_id=? AND model=?");
    bind_text(del.s, 1, provider);
    bind_text(del.s, 2, model);
    sqlite3_step(del.s);
}

std::optional<size_t> ModelStore::get_ctx(const std::string& provider, const std::string& model) const {
    std::lock_guard<std::mutex> lk(mtx_);
    Stmt s(db_, "SELECT context_window FROM model_ctx WHERE provider_id=? AND model=?");
    bind_text(s.s, 1, provider);
    bind_text(s.s, 2, model);
    if (sqlite3_step(s.s) != SQLITE_ROW) return std::nullopt;
    return static_cast<size_t>(sqlite3_column_int64(s.s, 0));
}

std::vector<std::pair<std::string, size_t>> ModelStore::ctx_map(const std::string& provider) const {
    std::lock_guard<std::mutex> lk(mtx_);
    std::vector<std::pair<std::string, size_t>> out;
    Stmt s(db_, "SELECT model,context_window FROM model_ctx WHERE provider_id=?");
    bind_text(s.s, 1, provider);
    while (sqlite3_step(s.s) == SQLITE_ROW) {
        out.emplace_back(reinterpret_cast<const char*>(sqlite3_column_text(s.s, 0)),
                         static_cast<size_t>(sqlite3_column_int64(s.s, 1)));
    }
    return out;
}

void ModelStore::set_ctx(const std::string& provider, const std::string& model, size_t window) {
    std::lock_guard<std::mutex> lk(mtx_);
    Stmt up(db_, "INSERT INTO model_ctx(provider_id,model,context_window) VALUES(?,?,?) "
                 "ON CONFLICT(provider_id,model) DO UPDATE SET context_window=excluded.context_window");
    bind_text(up.s, 1, provider);
    bind_text(up.s, 2, model);
    sqlite3_bind_int64(up.s, 3, static_cast<sqlite3_int64>(window));
    if (sqlite3_step(up.s) != SQLITE_DONE) die(db_, "upsert model_ctx: ");
}

void ModelStore::clear_ctx(const std::string& provider, const std::string& model) {
    std::lock_guard<std::mutex> lk(mtx_);
    Stmt del(db_, "DELETE FROM model_ctx WHERE provider_id=? AND model=?");
    bind_text(del.s, 1, provider);
    bind_text(del.s, 2, model);
    sqlite3_step(del.s);
}

} // namespace hermes
