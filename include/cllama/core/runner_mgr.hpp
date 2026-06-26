#ifndef CLLAMA_CORE_RUNNER_MGR_HPP
#define CLLAMA_CORE_RUNNER_MGR_HPP

#include "cllama/core/types.hpp"
#include "cllama/core/error.hpp"
#include <string>
#include <map>
#include <memory>
#include <mutex>
#include <chrono>
#include <functional>
#include <httplib.h>
#include <spdlog/spdlog.h>

namespace cllama {

/// Manages a pool of child runner processes.
/// Each runner loads a single model and exposes a local HTTP API.
/// The server spawns runners on demand and proxies inference requests to them.
enum class RunnerType {
    CLLAMA   ///< cllama_runner — our built-in runner binary
};

inline const char* to_string(RunnerType t) {
    switch (t) {
        case RunnerType::CLLAMA: return "cllama";
    }
    return "unknown";
}

class RunnerManager {
public:
    /// @param runner_binary  default path to the runner executable (e.g., cllama_runner)
    /// @param models_path    directory containing .gguf files
    explicit RunnerManager(std::string runner_binary, std::string models_path);

    ~RunnerManager();

    // ── runner registry ────────────────────────────────────────

    /// Register a non-default runner binary for a given model name.
    /// When the model is started, this binary will be used instead of the default.
    void register_runner_binary(const std::string& model_name, const std::string& runner_binary);

    /// Look up the runner binary for a model — returns the per-model override
    /// if registered, otherwise the default.
    std::string runner_for_model(const std::string& model_name) const;

    /// Register a runner binary for a given type name (e.g. "cllama" → /usr/bin/cllama_runner).
    void register_runner_type(const std::string& type_name, const std::string& runner_binary);

    /// Look up the runner binary for a type name. Returns empty if not registered.
    std::string binary_for_type(const std::string& type_name) const;

    // ── lifecycle ──────────────────────────────────────────────

    /// Ensure a runner is running for the given model name (uses default or per-model binary + resolved path).
    CLlamaResult<bool> start(const std::string& model_name);

    /// Explicitly start a runner under the given name with a specific binary and model path.
    CLlamaResult<bool> start_runner(const std::string& runner_name,
                                     const std::string& runner_binary,
                                     const std::string& runner_type,
                                     const std::string& model_path);

    /// Stop a runner by runner name (SIGTERM + wait).
    void stop(const std::string& runner_name);

    /// Stop all runners.
    void stop_all();

    // ── inference (proxy to runner) ────────────────────────────

    CLlamaResult<std::string> generate_completion(
        const std::string& model_name,
        const std::string& prompt,
        const CompletionOptions& opts);

    CLlamaResult<std::string> chat_completion(
        const std::string& model_name,
        const std::vector<Message>& messages,
        const CompletionOptions& opts);

    CLlamaResult<Embedding> generate_embeddings(
        const std::string& model_name,
        const std::string& input);

    CLlamaResult<bool> health(const std::string& model_name);

    /// List models found in the models directory (no runner needed).
    CLlamaResult<std::vector<ModelInfo>> list_models();

    /// Resolve a model name to a full .gguf path using the models directory.
    std::string resolve_model_path(const std::string& model_name) const;

    /// List active runners.
    struct RunnerInfo {
        std::string                          name;
        std::string                          runner_binary;
        std::string                          model_name;
        RunnerType                           runner_type = RunnerType::CLLAMA;
        int                                  port      = 0;
        int                                  pid       = -1;
        std::string                          model_path;
        std::chrono::system_clock::time_point started_at;
    };
    CLlamaResult<std::vector<RunnerInfo>> list_runners();

private:
    struct Runner {
        RunnerInfo                         info;
        std::unique_ptr<httplib::Client>   client;
    };

    Runner* get_or_start(const std::string& model_name);
    Runner* find(const std::string& name);
    Runner* spawn_runner(const std::string& name,
                         const std::string& binary,
                         const std::string& model_path,
                         int port);
    int     find_free_port();
    std::string resolve_path(const std::string& model_name) const;

    std::string runner_binary_;
    std::string models_path_;
    std::map<std::string, std::unique_ptr<Runner>> runners_;
    std::map<std::string, std::string> model_runner_map_;
    std::map<std::string, std::string> type_runner_map_;
    std::mutex mtx_;
    std::shared_ptr<spdlog::logger> log_;
};

} // namespace cllama

#endif
