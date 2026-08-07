#pragma once
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <vector>
#include <sqlite3.h>
#include "hermes/message.hpp"

namespace hermes {

struct SessionMeta {
    std::string id, title, provider, model, created_at;
};

struct Session {
    std::string id, title, provider, model, created_at;
    std::vector<Message> messages;
};

// SQLite 持久化会话:表 sessions(id, title, provider, model, created_at, messages-JSON)。
class SessionStore {
public:
    explicit SessionStore(const std::filesystem::path& db_path);
    ~SessionStore();
    SessionStore(const SessionStore&) = delete;
    SessionStore& operator=(const SessionStore&) = delete;

    std::vector<SessionMeta> list() const;
    std::optional<Session> get(const std::string& id) const;
    Session create(const std::string& provider, const std::string& model, const std::string& title);
    void update_model(const std::string& id, const std::string& model);
    void append_message(const std::string& id, const Message& msg);
    void replace_messages(const std::string& id, const std::vector<Message>& msgs);

private:
    std::optional<Session> get_unlocked(const std::string& id) const;
    void replace_unlocked(const std::string& id, const std::vector<Message>& msgs);

    sqlite3* db_ = nullptr;
    mutable std::mutex mtx_;
};

} // namespace hermes
