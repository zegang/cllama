#include "cllama/core/runner_mgr.hpp"
#include <cllama/platform/os.hpp>
#include <cllama/util/log.hpp>

namespace plat = cllama::platform;
#include <cstdio>
#include <thread>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <sstream>
#include <nlohmann/json.hpp>

using json = nlohmann::json;

namespace cllama {

// ── helpers ──────────────────────────────────────────────────────

static bool has_suffix(const std::string& s, const std::string& suf) {
    return s.size() >= suf.size() &&
           s.compare(s.size() - suf.size(), suf.size(), suf) == 0;
}

static json opts_to_json(const CompletionOptions& o) {
    return {
        {"max_tokens",  o.max_tokens},
        {"temperature", o.temperature},
        {"top_p",       o.top_p},
        {"top_k",       o.top_k}
    };
}

// ── ctor / dtor ──────────────────────────────────────────────────

RunnerManager::RunnerManager(std::string runner_binary, std::string models_path)
    : runner_binary_(std::move(runner_binary))
    , models_path_(std::move(models_path))
    , log_(cllama::util::get_logger("runner-mgr"))
{
    log_->info("RunnerManager created  binary={}  models={}", runner_binary_, models_path_);
}

RunnerManager::~RunnerManager() { stop_all(); }

// ── port finding ─────────────────────────────────────────────────

int RunnerManager::find_free_port() {
    return plat::find_free_port();
}

// ── resolve path ─────────────────────────────────────────────────

std::string RunnerManager::resolve_model_path(const std::string& name) const {
    return resolve_path(name);
}

std::string RunnerManager::resolve_path(const std::string& name) const {
    if (has_suffix(name, ".gguf") && name[0] == '/')
        return name;
    std::string p = models_path_;
    if (!p.empty() && p.back() != '/') p.push_back('/');
    p += name;
    if (!has_suffix(p, ".gguf")) p += ".gguf";
    return p;
}

// ── runner registry ──────────────────────────────────────────────

void RunnerManager::register_runner_binary(const std::string& model_name, const std::string& runner_binary) {
    log_->info("register_runner('{}') → {}", model_name, runner_binary);
    model_runner_map_[model_name] = runner_binary;
}

std::string RunnerManager::runner_for_model(const std::string& model_name) const {
    auto it = model_runner_map_.find(model_name);
    if (it != model_runner_map_.end())
        return it->second;
    return runner_binary_;
}

void RunnerManager::register_runner_type(const std::string& type_name, const std::string& runner_binary) {
    log_->info("register_runner_type('{}') → {}", type_name, runner_binary);
    type_runner_map_[type_name] = runner_binary;
}

std::string RunnerManager::binary_for_type(const std::string& type_name) const {
    auto it = type_runner_map_.find(type_name);
    if (it != type_runner_map_.end())
        return it->second;
    log_->warn("binary_for_type('{}') — not registered", type_name);
    return {};
}

// ── spawn ────────────────────────────────────────────────────────

RunnerManager::Runner* RunnerManager::spawn_runner(
    const std::string& name,
    const std::string& binary,
    const std::string& model_path,
    int port)
{
    std::string port_str = std::to_string(port);
    std::vector<std::string> args = {
        "--model", model_path,
        "--port", port_str
    };
    log_->info("Spawning runner '{}'  binary={}  port={}  model={}", name, binary, port, model_path);
    int pid = plat::spawn_process(binary, args);
    if (pid < 0) {
        log_->error("Failed to spawn runner '{}'  pid={}", name, pid);
        return nullptr;
    }

    auto entry = std::make_unique<Runner>();
    entry->info.name          = name;
    entry->info.runner_binary = binary;
    entry->info.model_name    = std::filesystem::path(model_path).stem().string();
    entry->info.runner_type   = RunnerType::CLLAMA;
    entry->info.port          = port;
    entry->info.pid           = pid;
    entry->info.model_path    = model_path;
    entry->info.started_at    = std::chrono::system_clock::now();
    entry->client             = std::make_unique<httplib::Client>("127.0.0.1", port);

    log_->info("Waiting for runner '{}' health (pid={}, port={})", name, pid, port);
    bool ok = false;
    for (int i = 0; i < 100; ++i) {
        auto res = entry->client->Get("/health");
        if (res && res->status == 200) { ok = true; break; }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    if (!ok) {
        log_->error("Runner '{}' failed health check after 10s, killing", name);
        plat::kill_process(pid, plat::SIG_TERM);
        plat::wait_process(pid);
        return nullptr;
    }
    log_->info("Runner '{}' is healthy (pid={})", name, pid);

    auto* ptr = entry.get();
    runners_[name] = std::move(entry);
    return ptr;
}

RunnerManager::Runner* RunnerManager::get_or_start(const std::string& model_name) {
    if (auto* r = find(model_name)) {
        log_->info("Reusing existing runner for '{}'", model_name);
        return r;
    }

    auto path = resolve_path(model_name);
    if (!std::filesystem::exists(path)) {
        log_->warn("Model file not found: {}", path);
        return nullptr;
    }

    int port = find_free_port();
    if (port <= 0) {
        log_->error("Failed to find free port for '{}'", model_name);
        return nullptr;
    }

    return spawn_runner(model_name, runner_for_model(model_name), path, port);
}

RunnerManager::Runner* RunnerManager::find(const std::string& name) {
    auto it = runners_.find(name);
    return it != runners_.end() ? it->second.get() : nullptr;
}

// ── public lifecycle ─────────────────────────────────────────────

CLlamaResult<bool> RunnerManager::start(const std::string& model_name) {
    log_->info("start('{}')", model_name);
    std::lock_guard<std::mutex> lk(mtx_);
    auto* r = get_or_start(model_name);
    if (!r) {
        log_->error("start('{}') failed", model_name);
        return Error(ErrorCode::MODEL_ERROR,
                     "Failed to start runner for " + model_name);
    }
    log_->info("start('{}') succeeded  pid={}", model_name, r->info.pid);
    return true;
}

CLlamaResult<bool> RunnerManager::start_runner(
    const std::string& runner_name,
    const std::string& runner_binary,
    const std::string& runner_type,
    const std::string& model_path)
{
    log_->info("start_runner('{}')  binary={}  type={}  model={}", runner_name, runner_binary, runner_type, model_path);
    std::lock_guard<std::mutex> lk(mtx_);

    if (find(runner_name)) {
        log_->warn("start_runner('{}') — already running", runner_name);
        return Error(ErrorCode::ALREADY_EXISTS,
                     "Runner already running: " + runner_name);
    }

    if (!std::filesystem::exists(model_path)) {
        log_->error("start_runner('{}') — model not found: {}", runner_name, model_path);
        return Error(ErrorCode::NOT_FOUND,
                     "Model file not found: " + model_path);
    }

    std::string effective_binary = runner_binary;
    if (effective_binary.empty()) {
        auto model_name = std::filesystem::path(model_path).stem().string();
        effective_binary = runner_for_model(model_name);
        log_->info("start_runner('{}') — resolved binary from model: {}", runner_name, effective_binary);
    }
    if (effective_binary.empty() && !runner_type.empty()) {
        effective_binary = binary_for_type(runner_type);
        log_->info("start_runner('{}') — resolved binary from type '{}': {}", runner_name, runner_type, effective_binary);
    }

    int port = find_free_port();
    if (port <= 0) {
        log_->error("start_runner('{}') — no free ports available", runner_name);
        return Error(ErrorCode::MODEL_ERROR, "Failed to find free port");
    }

    auto* r = spawn_runner(runner_name, effective_binary, model_path, port);
    if (!r) {
        log_->error("start_runner('{}') — spawn failed", runner_name);
        return Error(ErrorCode::MODEL_ERROR,
                     "Failed to start runner: " + runner_name);
    }
    log_->info("start_runner('{}') succeeded  pid={}", runner_name, r->info.pid);
    return true;
}

static void stop_with_timeout(int pid, spdlog::logger* log) {
    plat::kill_process(pid, plat::SIG_TERM);
    for (int i = 0; i < 50; ++i) {
        if (!plat::process_is_alive(pid)) {
            plat::wait_process(pid);
            return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    log->warn("Runner pid={} did not respond to SIGTERM, sending SIGKILL", pid);
    plat::kill_process(pid, plat::SIG_KILL);
    plat::wait_process(pid);
}

void RunnerManager::stop(const std::string& runner_name) {
    log_->info("stop('{}')", runner_name);
    std::lock_guard<std::mutex> lk(mtx_);
    auto it = runners_.find(runner_name);
    if (it == runners_.end()) {
        log_->warn("stop('{}') — not running", runner_name);
        return;
    }
    int pid = it->second->info.pid;
    log_->info("Killing runner '{}' (pid={})", runner_name, pid);
    stop_with_timeout(pid, log_.get());
    log_->info("Runner '{}' stopped and reaped", runner_name);
    runners_.erase(it);
}

void RunnerManager::stop_all() {
    log_->info("stop_all() — stopping {} runner(s)", runners_.size());
    std::lock_guard<std::mutex> lk(mtx_);
    // first pass: SIGTERM all runners in parallel
    for (auto& [name, r] : runners_) {
        log_->info("Sending SIGTERM to runner '{}' (pid={})", name, r->info.pid);
        plat::kill_process(r->info.pid, plat::SIG_TERM);
    }
    // second pass: wait for each with timeout + SIGKILL fallback
    for (auto& [name, r] : runners_) {
        log_->info("Waiting for runner '{}' (pid={})", name, r->info.pid);
        stop_with_timeout(r->info.pid, log_.get());
    }
    runners_.clear();
    log_->info("stop_all() done");
}

// ── proxy helpers ────────────────────────────────────────────────

#define PROXY_GUARD(runner_var, model_name)               \
    auto* runner_var = get_or_start(model_name);          \
    if (!runner_var)                                      \
        return Error(ErrorCode::MODEL_ERROR,               \
                     "Failed to start runner for " + model_name); \
    auto* cli = runner_var->client.get();

// ── NDJSON helpers ───────────────────────────────────────────────

static CLlamaResult<std::string> parse_ndjson(const std::string& body) {
    std::istringstream stream(body);
    std::string line;
    std::string text;
    while (std::getline(stream, line)) {
        if (line.empty()) continue;
        try {
            auto j = json::parse(line);
            if (j.contains("error"))
                return Error(ErrorCode::MODEL_ERROR, j["error"].get<std::string>());
            if (j.value("done", false))
                text = j.value("text", text);
        } catch (...) {
            return Error(ErrorCode::MODEL_ERROR, "Invalid NDJSON from runner");
        }
    }
    return text;
}

// ── stream NDJSON parser ─────────────────────────────────────────

struct StreamParseResult {
    std::string text;
    bool        ok = true;
    std::string error;
};

/// Shared state for streaming NDJSON parsing across httplib ContentReceiver chunks.
struct StreamCtx {
    std::string             leftover;
    std::string             full_text;
    bool                    got_error = false;
    std::string             error_msg;
    bool                    got_done  = false;
    RunnerManager::TokenCallback on_token;
};

static bool stream_parse_chunk(const char* data, size_t len, StreamCtx& ctx) {
    ctx.leftover.append(data, len);
    size_t pos = 0;
    while (true) {
        auto nl = ctx.leftover.find('\n', pos);
        if (nl == std::string::npos) break;
        std::string line = ctx.leftover.substr(pos, nl - pos);
        pos = nl + 1;

        if (line.empty()) continue;
        try {
            auto j = nlohmann::json::parse(line);
            if (j.contains("error")) {
                ctx.got_error = true;
                ctx.error_msg = j["error"].get<std::string>();
                return false;
            }
            if (j.contains("token") && ctx.on_token) {
                auto t = j["token"].get<std::string>();
                if (!ctx.on_token(t)) return false; // client aborted
            }
            if (j.value("done", false)) {
                ctx.got_done  = true;
                ctx.full_text = j.value("text", ctx.full_text);
                // keep parsing in case there's more
            }
        } catch (...) {
            ctx.got_error = true;
            ctx.error_msg = "Invalid NDJSON from runner";
            return false;
        }
    }
    ctx.leftover.erase(0, pos);
    return true;
}

// ── inference proxying ───────────────────────────────────────────

CLlamaResult<std::string> RunnerManager::generate_completion(
    const std::string& model_name,
    const std::string& prompt,
    const CompletionOptions& opts)
{
    log_->info("generate_completion('{}')  prompt_len={}", model_name, prompt.size());
    std::lock_guard<std::mutex> lk(mtx_);
    PROXY_GUARD(r, model_name);

    json body;
    body["prompt"]        = prompt;
    body["options"]       = opts_to_json(opts);

    auto res = cli->Post("/api/completion", body.dump(), "application/json");
    if (!res || res->status != 200) {
        int code = res ? res->status : 0;
        log_->error("generate_completion('{}') — HTTP {}", model_name, code);
        return Error(ErrorCode::NETWORK_ERROR,
                     "Runner request failed: HTTP " + std::to_string(code));
    }

    auto result = parse_ndjson(res->body);
    if (result.success())
        log_->info("generate_completion('{}') success  size={}", model_name, result.get().size());
    else
        log_->error("generate_completion('{}') failed: {}", model_name, result.error().message);
    return result;
}

CLlamaResult<std::string> RunnerManager::chat_completion(
    const std::string& model_name,
    const std::vector<Message>& messages,
    const CompletionOptions& opts)
{
    log_->info("chat_completion('{}')  messages={}", model_name, messages.size());
    std::lock_guard<std::mutex> lk(mtx_);
    PROXY_GUARD(r, model_name);

    json msgs = json::array();
    for (const auto& m : messages) {
        msgs.push_back({{"role", m.role}, {"content", m.content}});
    }

    json body;
    body["messages"]      = msgs;
    body["options"]       = opts_to_json(opts);

    auto res = cli->Post("/api/chat", body.dump(), "application/json");
    if (!res || res->status != 200) {
        int code = res ? res->status : 0;
        log_->error("chat_completion('{}') — HTTP {}", model_name, code);
        return Error(ErrorCode::NETWORK_ERROR,
                     "Runner chat failed: HTTP " + std::to_string(code));
    }

    auto result = parse_ndjson(res->body);
    if (result.success())
        log_->info("chat_completion('{}') success  size={}", model_name, result.get().size());
    else
        log_->error("chat_completion('{}') failed: {}", model_name, result.error().message);
    return result;
}

// ── streaming inference ─────────────────────────────────────────

CLlamaResult<std::string> RunnerManager::stream_completion(
    const std::string& model_name,
    const std::string& prompt,
    const CompletionOptions& opts,
    TokenCallback on_token)
{
    log_->info("stream_completion('{}')  prompt_len={}", model_name, prompt.size());
    std::lock_guard<std::mutex> lk(mtx_);
    PROXY_GUARD(r, model_name);

    json body;
    body["prompt"]  = prompt;
    body["options"] = opts_to_json(opts);

    StreamCtx ctx;
    ctx.on_token = std::move(on_token);

    httplib::Request req;
    req.method = "POST";
    req.path = "/api/completion";
    req.set_header("Content-Type", "application/json");
    req.body = body.dump();
    req.content_receiver = [&](const char* data, size_t data_len, uint64_t, uint64_t) {
        return stream_parse_chunk(data, data_len, ctx);
    };

    auto res = cli->send(req);

    if (!res) {
        log_->error("stream_completion('{}') — connection failed", model_name);
        return Error(ErrorCode::NETWORK_ERROR, "Runner connection failed");
    }
    if (res->status != 200) {
        log_->error("stream_completion('{}') — HTTP {}", model_name, res->status);
        return Error(ErrorCode::NETWORK_ERROR,
                     "Runner request failed: HTTP " + std::to_string(res->status));
    }
    if (ctx.got_error) {
        log_->error("stream_completion('{}') — runner error: {}", model_name, ctx.error_msg);
        return Error(ErrorCode::MODEL_ERROR, ctx.error_msg);
    }

    log_->info("stream_completion('{}') success  size={}", model_name, ctx.full_text.size());
    return ctx.full_text;
}

CLlamaResult<std::string> RunnerManager::stream_chat(
    const std::string& model_name,
    const std::vector<Message>& messages,
    const CompletionOptions& opts,
    TokenCallback on_token)
{
    log_->info("stream_chat('{}')  messages={}", model_name, messages.size());
    std::lock_guard<std::mutex> lk(mtx_);
    PROXY_GUARD(r, model_name);

    json msgs = json::array();
    for (const auto& m : messages)
        msgs.push_back({{"role", m.role}, {"content", m.content}});

    json body;
    body["messages"] = msgs;
    body["options"]  = opts_to_json(opts);

    StreamCtx ctx;
    ctx.on_token = std::move(on_token);

    httplib::Request req;
    req.method = "POST";
    req.path = "/api/chat";
    req.set_header("Content-Type", "application/json");
    req.body = body.dump();
    req.content_receiver = [&](const char* data, size_t data_len, uint64_t, uint64_t) {
        return stream_parse_chunk(data, data_len, ctx);
    };

    auto res = cli->send(req);

    if (!res) {
        log_->error("stream_chat('{}') — connection failed", model_name);
        return Error(ErrorCode::NETWORK_ERROR, "Runner connection failed");
    }
    if (res->status != 200) {
        log_->error("stream_chat('{}') — HTTP {}", model_name, res->status);
        return Error(ErrorCode::NETWORK_ERROR,
                     "Runner chat failed: HTTP " + std::to_string(res->status));
    }
    if (ctx.got_error) {
        log_->error("stream_chat('{}') — runner error: {}", model_name, ctx.error_msg);
        return Error(ErrorCode::MODEL_ERROR, ctx.error_msg);
    }

    log_->info("stream_chat('{}') success  size={}", model_name, ctx.full_text.size());
    return ctx.full_text;
}

CLlamaResult<Embedding> RunnerManager::generate_embeddings(
    const std::string& model_name,
    const std::string& input)
{
    log_->info("generate_embeddings('{}')  input_len={}", model_name, input.size());
    std::lock_guard<std::mutex> lk(mtx_);
    PROXY_GUARD(r, model_name);

    json body;
    body["input"] = input;

    auto res = cli->Post("/api/embeddings", body.dump(), "application/json");
    if (!res || res->status != 200) {
        int code = res ? res->status : 0;
        log_->error("generate_embeddings('{}') — HTTP {}", model_name, code);
        return Error(ErrorCode::NETWORK_ERROR,
                     "Runner embeddings failed: HTTP " + std::to_string(code));
    }

    try {
        auto j = json::parse(res->body);
        Embedding emb;
        emb.data      = j["embedding"].get<std::vector<float>>();
        emb.dimension = static_cast<int32_t>(emb.data.size());
        log_->info("generate_embeddings('{}') success  dim={}", model_name, emb.dimension);
        return emb;
    } catch (...) {
        log_->error("generate_embeddings('{}') — invalid response JSON", model_name);
        return Error(ErrorCode::MODEL_ERROR, "Invalid runner response");
    }
}

CLlamaResult<std::vector<ModelInfo>> RunnerManager::list_models() {
    std::vector<ModelInfo> models;
    if (!std::filesystem::is_directory(models_path_)) {
        log_->warn("list_models — models path is not a directory: {}", models_path_);
        return models;
    }
    for (const auto& entry : std::filesystem::directory_iterator(models_path_)) {
        if (!entry.is_regular_file()) continue;
        if (entry.path().extension() == ".gguf") {
            ModelInfo info;
            info.name     = entry.path().stem().string();
            info.model_path = entry.path().string();
            info.family   = "llama";
            info.size     = std::to_string(std::filesystem::file_size(entry.path()));
            info.file_format = "gguf";
            info.max_embedding_length = 8192;
            models.push_back(std::move(info));
        }
    }
    log_->info("list_models — found {} model(s)", models.size());
    return models;
}

CLlamaResult<std::vector<RunnerManager::RunnerInfo>> RunnerManager::list_runners() {
    std::lock_guard<std::mutex> lk(mtx_);
    std::vector<RunnerInfo> result;
    for (const auto& [name, r] : runners_) {
        result.push_back(r->info);
    }
    return result;
}

CLlamaResult<bool> RunnerManager::health(const std::string& model_name) {
    std::lock_guard<std::mutex> lk(mtx_);
    auto* r = find(model_name);
    if (!r) {
        log_->warn("health('{}') — not found", model_name);
        return false;
    }
    auto res = r->client->Get("/health");
    bool ok = res && res->status == 200;
    log_->info("health('{}') — status={}", model_name, ok ? "healthy" : "unhealthy");
    return ok;
}

} // namespace cllama
