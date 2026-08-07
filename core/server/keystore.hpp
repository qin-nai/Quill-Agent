#pragma once
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <sqlite3.h>

namespace hermes {

// SQLite 持久化 API key(开发期;移动端最终走平台 Keychain)。表 keys(provider_id, key_value)。
class KeyStore {
public:
    explicit KeyStore(const std::filesystem::path& db_path);
    ~KeyStore();
    KeyStore(const KeyStore&) = delete;
    KeyStore& operator=(const KeyStore&) = delete;

    // 空 key 视为删除。
    void set(const std::string& provider, const std::string& key);
    std::optional<std::string> get(const std::string& provider) const;
    bool has(const std::string& provider) const;

private:
    sqlite3* db_ = nullptr;
    mutable std::mutex mtx_;
};

} // namespace hermes
