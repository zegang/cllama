#include "cllama/api/config.hpp"
#include <cstdlib>
#include <cstring>
#include <charconv>
#include <system_error>

namespace cllama {
namespace api {

static const char* env(const char* key) {
    return std::getenv(key);
}

static int env_int(const char* key, int fallback) {
    auto* v = env(key);
    if (!v) return fallback;
    int val;
    auto res = std::from_chars(v, v + std::strlen(v), val);
    if (res.ec != std::errc{}) return fallback;
    return val;
}

ServerConfig ServerConfig::apply_env_overrides(ServerConfig cfg) {
    if (auto* v = env("CLLAMA_HOST"))                cfg.host = v;
    if (auto* v = env("CLLAMA_PORT"))                cfg.port = env_int("CLLAMA_PORT", cfg.port);
    if (auto* v = env("CLLAMA_MODEL_PATH"))           cfg.model_path = v;
    if (auto* v = env("CLLAMA_MAX_REQUEST_WORKERS"))  cfg.max_request_workers = env_int("CLLAMA_MAX_REQUEST_WORKERS", cfg.max_request_workers);
    if (auto* v = env("CLLAMA_REQUEST_QUEUE_MAX_SIZE")) cfg.request_queue_max_size = env_int("CLLAMA_REQUEST_QUEUE_MAX_SIZE", cfg.request_queue_max_size);
    if (auto* v = env("CLLAMA_READ_TIMEOUT"))          cfg.read_timeout_seconds = env_int("CLLAMA_READ_TIMEOUT", cfg.read_timeout_seconds);
    if (auto* v = env("CLLAMA_WRITE_TIMEOUT"))         cfg.write_timeout_seconds = env_int("CLLAMA_WRITE_TIMEOUT", cfg.write_timeout_seconds);
    if (auto* v = env("CLLAMA_KEEP_ALIVE_TIMEOUT"))    cfg.keep_alive_timeout_seconds = env_int("CLLAMA_KEEP_ALIVE_TIMEOUT", cfg.keep_alive_timeout_seconds);
    if (auto* v = env("CLLAMA_AUTH_TOKEN"))            cfg.authentication_token = v;

    if (auto* v = env("CLLAMA_ALLOWED_ORIGINS")) {
        cfg.allowed_origins.clear();
        std::string s = v;
        size_t pos = 0;
        while ((pos = s.find(',')) != std::string::npos) {
            cfg.allowed_origins.push_back(s.substr(0, pos));
            s.erase(0, pos + 1);
        }
        if (!s.empty()) cfg.allowed_origins.push_back(s);
    }

    return cfg;
}

} // namespace api
} // namespace cllama
