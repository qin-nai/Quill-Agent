#pragma once
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>
#include <sqlite3.h>

namespace hermes {

// 用户自定义模型持久化。内置模型在 provider.cpp 写死,自定义的存这里;
// 展示时两者合并(内置 ∪ 自定义),顶栏模型栏与设置里的模型管理都读合并结果。
class ModelStore {
public:
    explicit ModelStore(const std::filesystem::path& db_path);
    ~ModelStore();
    ModelStore(const ModelStore&) = delete;
    ModelStore& operator=(const ModelStore&) = delete;

    std::vector<std::string> list(const std::string& provider) const;
    void add(const std::string& provider, const std::string& model);
    void remove(const std::string& provider, const std::string& model);

    // 内置模型删除黑名单:内置模型被用户删除后记录在这里,展示时从内置列表排除
    std::vector<std::string> list_removed(const std::string& provider) const;
    void add_removed(const std::string& provider, const std::string& model);
    void clear_removed(const std::string& provider, const std::string& model);

    // 单模型上下文覆盖(默认 256k,勾选 1M 存 1000000,取消清空回默认)
    std::optional<size_t> get_ctx(const std::string& provider, const std::string& model) const;
    std::vector<std::pair<std::string, size_t>> ctx_map(const std::string& provider) const;
    void set_ctx(const std::string& provider, const std::string& model, size_t window);
    void clear_ctx(const std::string& provider, const std::string& model);

private:
    sqlite3* db_ = nullptr;
    mutable std::mutex mtx_;
};

} // namespace hermes
