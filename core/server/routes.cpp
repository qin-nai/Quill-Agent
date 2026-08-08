#include "routes.hpp"
#include "platform_thread.hpp"
#include "hermes/agent_loop.hpp"
#include "hermes/json_io.hpp"
#include "hermes/llm/llm_client.hpp"
#include "hermes/provider.hpp"
#include "http_transport_winhttp.hpp"
#include "hermes/tools/path_guard.hpp"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <thread>

namespace hermes {

namespace {

using json = nlohmann::json;

std::optional<json> parse_body(const httplib::Request& req) {
    try { return json::parse(req.body); } catch (...) { return std::nullopt; }
}

void set_error(httplib::Response& res, int code, const std::string& msg) {
    res.status = code;
    res.set_content(json{{"error", msg}}.dump(), "application/json");
}

json provider_json(const ProviderConfig& p) {
    const char* ad = p.adapter == AdapterType::Claude ? "claude"
                     : p.adapter == AdapterType::OpenAICompat ? "openai" : "stub";
    return json{{"id", p.id}, {"name", p.name}, {"adapter", ad}, {"base_url", p.base_url},
                {"models", p.models}, {"default_model", p.default_model},
                {"context_window", p.context_window}};
}

json session_meta_json(const SessionMeta& m) {
    return json{{"id", m.id}, {"title", m.title}, {"provider", m.provider},
                {"model", m.model}, {"created_at", m.created_at}};
}

// 文件树:节点带 workspace 相对路径(posix 分隔),排除 data/ 与 denylist,深度上限 8。
json build_tree(const std::filesystem::path& dir, const std::string& rel_prefix, int depth) {
    json arr = json::array();
    std::error_code ec;
    std::vector<std::filesystem::path> entries;
    for (auto it = std::filesystem::directory_iterator(dir, ec);
         !ec && it != std::filesystem::directory_iterator{}; it.increment(ec)) {
        entries.push_back(it->path());
    }
    std::sort(entries.begin(), entries.end());
    for (const auto& p : entries) {
        const std::string name = p.filename().string();
        const std::string rel = rel_prefix.empty() ? name : rel_prefix + "/" + name;
        std::error_code pec;
        if (std::filesystem::is_directory(p, pec)) {
            if (name == "data" || name == ".git" || name == "build" || name == "node_modules") continue;
            arr.push_back(json{{"name", name}, {"type", "dir"}, {"path", rel},
                               {"children", depth >= 8 ? json::array()
                                                       : build_tree(p, rel, depth + 1)}});
        } else if (std::filesystem::is_regular_file(p, pec)) {
            arr.push_back(json{{"name", name}, {"type", "file"}, {"path", rel},
                               {"size", static_cast<long long>(std::filesystem::file_size(p, pec))}});
        }
    }
    return arr;
}

// data/ 目录含 SQLite 库(key 明文),文件接口一律封禁。
bool path_blocked(const std::string& rel) {
    auto first = std::filesystem::path(rel).begin();
    return first != std::filesystem::path(rel).end() && *first == "data";
}

} // namespace

void register_routes(httplib::Server& svr, AppContext& ctx) {
    svr.Get("/api/health", [](const httplib::Request&, httplib::Response& res) {
        res.set_content(json{{"ok", true}}.dump(), "application/json");
    });

    svr.Get("/api/providers", [&ctx](const httplib::Request&, httplib::Response& res) {
        json arr = json::array();
        for (const auto& p : builtin_providers()) {
            json j = provider_json(p);
            const auto removed = ctx.models.list_removed(p.id);
            // 内置 − 已删除黑名单
            json builtin = json::array();
            for (const auto& m : j["models"])
                if (std::find(removed.begin(), removed.end(), m.get<std::string>()) == removed.end())
                    builtin.push_back(m);
            j["models"] = builtin;
            // 合并自定义模型
            j["custom_models"] = ctx.models.list(p.id);
            for (const auto& m : j["custom_models"])
                if (std::find(j["models"].begin(), j["models"].end(), m) == j["models"].end())
                    j["models"].push_back(m);
            j["removed_models"] = removed;
            // 每模型的上下文覆盖(勾选 1M 后为 1000000,默认 256k 不在此列)
            j["model_ctx"] = json::object();
            for (const auto& [m, w] : ctx.models.ctx_map(p.id)) j["model_ctx"][m] = w;
            arr.push_back(std::move(j));
        }
        res.set_content(arr.dump(), "application/json");
    });

    // 模型管理:自定义添加 / 删除。内置模型删除 → 记入黑名单(隐藏);自定义 → 真删。
    svr.Post(R"(/api/providers/([^/]+)/models)", [&ctx](const httplib::Request& req, httplib::Response& res) {
        auto body = parse_body(req);
        if (!body || !body->contains("model")) return set_error(res, 400, "body 需含 model");
        const std::string model = (*body)["model"].get<std::string>();
        if (model.empty()) return set_error(res, 400, "empty model");
        ctx.models.add(req.matches[1], model);
        res.set_content(json{{"ok", true}}.dump(), "application/json");
    });
    svr.Post(R"(/api/providers/([^/]+)/models/remove)", [&ctx](const httplib::Request& req, httplib::Response& res) {
        auto body = parse_body(req);
        if (!body || !body->contains("model")) return set_error(res, 400, "body 需含 model");
        const std::string provider = req.matches[1];
        const std::string model = (*body)["model"].get<std::string>();
        bool is_builtin = false;
        for (const auto& p : builtin_providers()) {
            if (p.id != provider) continue;
            for (const auto& m : p.models)
                if (m == model) { is_builtin = true; break; }
        }
        if (is_builtin) ctx.models.add_removed(provider, model);
        else ctx.models.remove(provider, model);
        res.set_content(json{{"ok", true}}.dump(), "application/json");
    });
    // 单模型上下文覆盖:context_window>0 存覆盖,=0 清除回默认(256k)
    svr.Post(R"(/api/providers/([^/]+)/models/ctx)", [&ctx](const httplib::Request& req, httplib::Response& res) {
        auto body = parse_body(req);
        if (!body || !body->contains("model") || !body->contains("context_window"))
            return set_error(res, 400, "body 需含 model 和 context_window");
        const std::string model = (*body)["model"].get<std::string>();
        const size_t window = (*body)["context_window"].get<size_t>();
        if (window == 0) ctx.models.clear_ctx(req.matches[1], model);
        else ctx.models.set_ctx(req.matches[1], model, window);
        res.set_content(json{{"ok", true}}.dump(), "application/json");
    });

    svr.Post(R"(/api/providers/([^/]+)/key)", [&ctx](const httplib::Request& req, httplib::Response& res) {
        auto body = parse_body(req);
        if (!body || !body->contains("key")) return set_error(res, 400, "body 需含 key");
        ctx.keys.set(req.matches[1], (*body)["key"].get<std::string>());
        res.set_content(json{{"ok", true}}.dump(), "application/json");
    });

    // 回读已保存的 key(供设置抽屉打开时回填);未配置返回空串
    svr.Get(R"(/api/providers/([^/]+)/key)", [&ctx](const httplib::Request& req, httplib::Response& res) {
        auto k = ctx.keys.get(req.matches[1]);
        res.set_content(json{{"key", k.value_or("")}}.dump(), "application/json");
    });

    // 连通性检测:最小非流式 1-token 请求(不复用 adapter,避免烧钱)。
    svr.Post(R"(/api/providers/([^/]+)/test)", [&ctx](const httplib::Request& req, httplib::Response& res) {
        const std::string id = req.matches[1];
        std::optional<ProviderConfig> found;
        for (const auto& p : builtin_providers())
            if (p.id == id) { found = p; break; }
        if (!found) return set_error(res, 404, "unknown provider");

        auto body = parse_body(req);
        std::string key = body && body->contains("key") ? (*body)["key"].get<std::string>() : "";
        if (key.empty()) {
            auto saved = ctx.keys.get(id);
            if (!saved) return set_error(res, 400, "missing key");
            key = *saved;
        }

        ProviderConfig cfg = *found;
        if (cfg.default_model.empty() && !cfg.models.empty()) cfg.default_model = cfg.models[0];
        if (cfg.default_model.empty()) return set_error(res, 400, "missing model");
        cfg.api_key_ref = key;

        std::atomic<bool> abort{false};
        auto transport = make_http_transport(abort);
        json msgs = json::array();
        msgs.push_back({{"role", "user"}, {"content", "ping"}});
        json reqbody = {{"model", cfg.default_model}, {"max_tokens", 1}, {"stream", false},
                        {"messages", msgs}};
        HttpRequest hr;
        hr.method = "POST";
        hr.body = reqbody.dump();
        if (cfg.adapter == AdapterType::Claude) {
            hr.url = cfg.base_url + "/v1/messages";
            hr.headers = {{"content-type", "application/json"}, {"x-api-key", key},
                          {"anthropic-version", "2023-06-01"}};
        } else {
            hr.url = cfg.base_url + "/chat/completions";
            hr.headers = {{"content-type", "application/json"},
                          {"authorization", "Bearer " + key}};
        }

        const auto t0 = std::chrono::steady_clock::now();
        const HttpResponse rsp = transport(hr, [](const std::string&) {});
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now() - t0).count();
        if (rsp.status == 0) return set_error(res, 502, rsp.error.empty() ? "network error" : rsp.error);
        if (rsp.status == 200) {
            res.set_content(json{{"ok", true}, {"latency_ms", ms}}.dump(), "application/json");
        } else if (rsp.status == 401 || rsp.status == 403) {
            set_error(res, 401, "key invalid");
        } else {
            set_error(res, 502, "HTTP " + std::to_string(rsp.status));
        }
    });

    svr.Get("/api/sessions", [&ctx](const httplib::Request&, httplib::Response& res) {
        json arr = json::array();
        for (const auto& m : ctx.sessions.list()) arr.push_back(session_meta_json(m));
        res.set_content(arr.dump(), "application/json");
    });

    svr.Post("/api/sessions", [&ctx](const httplib::Request& req, httplib::Response& res) {
        auto body = parse_body(req);
        const std::string provider = body ? body->value("provider", "deepseek") : "deepseek";
        std::string model = body ? body->value("model", "") : "";
        const std::string title = body ? body->value("title", "") : "";
        if (model.empty()) {
            for (const auto& p : builtin_providers())
                if (p.id == provider) { model = p.default_model; break; }
        }
        const Session s = ctx.sessions.create(provider, model, title);
        res.set_content(json{{"session", session_meta_json(
            SessionMeta{s.id, s.title, s.provider, s.model, s.created_at})}}.dump(), "application/json");
    });

    // 删除会话:运行中 409;不存在 404;成功 {"ok":true}
    svr.Delete(R"(/api/sessions/([^/]+))", [&ctx](const httplib::Request& req, httplib::Response& res) {
        const std::string sid = req.matches[1];
        {
            std::lock_guard<std::mutex> lk(ctx.active_mtx);
            if (ctx.active.count(sid)) return set_error(res, 409, "session busy");
            ctx.active.erase(sid);  // 防御性清理(按 busy 语义本应不在 active)
        }
        if (!ctx.sessions.remove(sid)) return set_error(res, 404, "session not found");
        res.set_content(json{{"ok", true}}.dump(), "application/json");
    });

    svr.Get(R"(/api/sessions/([^/]+)/messages)", [&ctx](const httplib::Request& req, httplib::Response& res) {
        auto s = ctx.sessions.get(req.matches[1]);
        if (!s) return set_error(res, 404, "session not found");
        json j;
        j["id"] = s->id; j["title"] = s->title; j["provider"] = s->provider;
        j["model"] = s->model; j["created_at"] = s->created_at;
        j["messages"] = s->messages;  // Message 经 json_io ADL 序列化
        res.set_content(j.dump(), "application/json");
    });

    // SSE 回合:worker 线程跑 AgentLoop,事件经 RunContext 队列转发为 SSE。
    svr.Post(R"(/api/sessions/([^/]+)/messages)", [&ctx](const httplib::Request& req, httplib::Response& res) {
        const std::string sid = req.matches[1];
        auto body = parse_body(req);
        const std::string text = body ? body->value("text", "") : "";
        const std::string model = body ? body->value("model", "") : "";
        if (text.empty()) return set_error(res, 400, "empty text");

        auto session = ctx.sessions.get(sid);
        if (!session) return set_error(res, 404, "session not found");

        // 前端模型栏切换 → 用请求里带的 model 覆盖会话模型并持久化,切换立即生效
        if (!model.empty() && model != session->model) {
            ctx.sessions.update_model(sid, model);
            session->model = model;
        }

        {
            std::lock_guard<std::mutex> lk(ctx.active_mtx);
            if (ctx.active.count(sid)) return set_error(res, 409, "session busy");
        }

        auto cfg = resolve_config(ctx, session->provider, session->model);
        if (!cfg) return set_error(res, 400, "该 provider 未配置 API Key");

        Message um;
        um.role = "user";
        um.content.push_back(ContentBlock::make_text(text));
        ctx.sessions.append_message(sid, um);

        auto rc = std::make_shared<RunContext>();
        {
            std::lock_guard<std::mutex> lk(ctx.active_mtx);
            ctx.active[sid] = rc;
        }

        std::thread([&ctx, sid, rc, cfg = *cfg]() mutable {
            platform_thread_enter();  // Android:AttachCurrentThread(native 线程需先附加才能调 JNI)
            try {
                auto ses = ctx.sessions.get(sid);
                std::vector<Message> history = ses ? ses->messages : std::vector<Message>{};
                // LLM 流式:连接 30s;相邻数据块空闲超时 300s(thinking 模型思考停顿可能远超 60s)
                LLMClient client(cfg, make_http_transport(rc->abort, 30000, 300000));
                AgentLoop loop(ctx.tools, ctx.workspace_root, ctx.user_skills_root);
                loop.run(cfg, client.adapter(), history,
                         [rc](const ContentBlock&) { return rc->wait_confirm(); },
                         [rc](const AgentEvent& e) { rc->queue->push(e); });
                ctx.sessions.replace_messages(sid, history);
            } catch (const std::exception& e) {
                // 任何异常都不能让 SSE 挂死:上报错误,走正常收尾
                rc->queue->push(AgentEvent::make_err(std::string("内部错误: ") + e.what()));
            } catch (...) {
                rc->queue->push(AgentEvent::make_err("内部错误: 未知异常"));
            }
            {
                std::lock_guard<std::mutex> lk(ctx.active_mtx);
                ctx.active.erase(sid);
            }
            rc->queue->push(AgentEvent::make_close());
            platform_thread_exit();  // Android:DetachCurrentThread(附加后退出前必须 detach)
        }).detach();

        res.set_header("Cache-Control", "no-cache");
        res.set_header("Connection", "keep-alive");
        res.set_chunked_content_provider("text/event-stream", make_sse_provider(rc), nullptr);
    });

    svr.Post(R"(/api/sessions/([^/]+)/confirm)", [&ctx](const httplib::Request& req, httplib::Response& res) {
        RunContextPtr rc;
        {
            std::lock_guard<std::mutex> lk(ctx.active_mtx);
            auto it = ctx.active.find(req.matches[1]);
            if (it == ctx.active.end()) return set_error(res, 404, "no active run");
            rc = it->second;
        }
        auto body = parse_body(req);
        const bool allow = body && body->contains("allow") ? (*body)["allow"].get<bool>() : false;
        const std::string reason = body ? body->value("reason", "") : "";
        rc->answer(allow, reason);
        res.set_content(json{{"ok", true}}.dump(), "application/json");
    });

    svr.Post(R"(/api/sessions/([^/]+)/stop)", [&ctx](const httplib::Request& req, httplib::Response& res) {
        RunContextPtr rc;
        {
            std::lock_guard<std::mutex> lk(ctx.active_mtx);
            auto it = ctx.active.find(req.matches[1]);
            if (it == ctx.active.end()) return set_error(res, 404, "no active run");
            rc = it->second;
        }
        rc->cancel();
        res.set_content(json{{"ok", true}}.dump(), "application/json");
    });

    svr.Get("/api/files/tree", [&ctx](const httplib::Request&, httplib::Response& res) {
        json root;
        if (std::filesystem::exists(ctx.workspace_root)) {
            root = build_tree(ctx.workspace_root, "", 0);
        }
        res.set_content(json{{"root", root}}.dump(), "application/json");
    });

    svr.Get("/api/files/content", [&ctx](const httplib::Request& req, httplib::Response& res) {
        const std::string rel = req.get_param_value("path");
        if (path_blocked(rel)) return set_error(res, 403, "blocked");
        auto full = resolve_within_root(ctx.workspace_root, rel);
        if (!full) return set_error(res, 400, "path 越界");
        std::error_code ec;
        const auto sz = std::filesystem::file_size(*full, ec);
        if (ec) return set_error(res, 404, "file not found");
        if (sz > 1024 * 1024) return set_error(res, 413, "file too large");
        std::ifstream ifs(*full, std::ios::binary);
        if (!ifs) return set_error(res, 404, "file not found");
        std::ostringstream ss;
        ss << ifs.rdbuf();
        res.set_content(json{{"ok", true}, {"content", ss.str()}}.dump(), "application/json");
    });
}

} // namespace hermes
