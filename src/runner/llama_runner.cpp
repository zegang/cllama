#include <string>
#include <sstream>
#include <vector>
#include <deque>
#include <random>
#include <cstring>
#include <csignal>
#include <functional>
#include <thread>
#include <httplib.h>
#include <nlohmann/json.hpp>
#include <cllama/util/log.hpp>
#include <spdlog/spdlog.h>

#include "llama.h"

using json = nlohmann::json;
static volatile std::sig_atomic_t g_running = 1;
static httplib::Server*           g_svr    = nullptr;

static void signal_handler(int) {
    g_running = 0;
    if (g_svr) g_svr->stop();
}

// ── helpers ──────────────────────────────────────────────────────

struct CompletionOpts {
    int   max_tokens  = 256;
    float temperature = 0.7f;
    float top_p       = 0.9f;
    int   top_k       = 40;
    int   seed        = -1;
};

static CompletionOpts opts_from_json(const json& j) {
    CompletionOpts o;
    if (j.contains("max_tokens"))  o.max_tokens  = j["max_tokens"].get<int>();
    if (j.contains("temperature")) o.temperature = j["temperature"].get<float>();
    if (j.contains("top_p"))       o.top_p       = j["top_p"].get<float>();
    if (j.contains("top_k"))       o.top_k       = j["top_k"].get<int>();
    return o;
}

// ── generation ───────────────────────────────────────────────────

static std::string generate(::llama_model* model, const std::string& prompt,
                            const CompletionOpts& opts,
                            bool parse_special = false,
                            const std::function<void(const std::string&)>& on_token = nullptr)
{
    auto ctx_params = llama_context_default_params();
    ctx_params.n_ctx     = static_cast<uint32_t>(std::max(opts.max_tokens * 2, 2048));
    ctx_params.n_threads = 4;
    auto* ctx = llama_init_from_model(model, ctx_params);
    if (!ctx) return "";

    const auto* vocab = llama_model_get_vocab(model);
    int32_t n_ctx_max = static_cast<int32_t>(llama_n_ctx(ctx));

    std::vector<llama_token> tokens(n_ctx_max);
    int32_t n_tokens = llama_tokenize(vocab, prompt.c_str(), static_cast<int32_t>(prompt.size()),
                                       tokens.data(), static_cast<int32_t>(tokens.size()), true, parse_special);
    if (n_tokens < 0) { llama_free(ctx); return ""; }
    tokens.resize(n_tokens);

    auto batch = llama_batch_get_one(tokens.data(), static_cast<int32_t>(tokens.size()));
    if (llama_decode(ctx, batch) != 0) { llama_free(ctx); return ""; }

    auto sparams = llama_sampler_chain_default_params();
    auto* smpl = llama_sampler_chain_init(sparams);
    llama_sampler_chain_add(smpl, llama_sampler_init_temp(opts.temperature));
    if (opts.top_p < 1.0f)
        llama_sampler_chain_add(smpl, llama_sampler_init_top_p(opts.top_p, 1));
    uint32_t seed = opts.seed >= 0
        ? static_cast<uint32_t>(opts.seed)
        : static_cast<uint32_t>(std::random_device{}());
    llama_sampler_chain_add(smpl, llama_sampler_init_dist(seed));

    int32_t max_tokens = opts.max_tokens > 0 ? opts.max_tokens : 256;
    std::string output;
    for (int32_t i = 0; i < max_tokens; ++i) {
        auto tok = llama_sampler_sample(smpl, ctx, -1);
        if (llama_vocab_is_eog(vocab, tok)) break;
        char piece[16];
        int32_t n = llama_token_to_piece(vocab, tok, piece, sizeof(piece), 0, false);
        if (n > 0) {
            output.append(piece, static_cast<size_t>(n));
            if (on_token) on_token(std::string(piece, static_cast<size_t>(n)));
        }
        batch = llama_batch_get_one(&tok, 1);
        if (llama_decode(ctx, batch) != 0) break;
    }

    llama_sampler_free(smpl);
    llama_free(ctx);
    return output;
}

static std::string chat(::llama_model* model, const json& messages_json,
                        const CompletionOpts& opts,
                        const std::string& chat_template = "",
                        const std::function<void(const std::string&)>& on_token = nullptr)
{
    std::vector<llama_chat_message> chat;
    std::vector<std::string> owned;
    owned.reserve(messages_json.size() * 2);
    for (const auto& jm : messages_json) {
        owned.push_back(jm["role"].get<std::string>());
        owned.push_back(jm["content"].get<std::string>());
    }
    for (size_t i = 0; i < owned.size(); i += 2)
        chat.push_back({owned[i].c_str(), owned[i + 1].c_str()});

    const char* tmpl = chat_template.empty() ? nullptr : chat_template.c_str();
    int32_t tmpl_len = llama_chat_apply_template(tmpl, chat.data(), chat.size(), true, nullptr, 0);
    std::string prompt(static_cast<size_t>(std::max(tmpl_len, 64)), '\0');
    llama_chat_apply_template(tmpl, chat.data(), chat.size(), true,
                               prompt.data(), static_cast<int32_t>(prompt.size()));
    prompt.resize(std::strlen(prompt.c_str()));

    return generate(model, prompt, opts, true, on_token);
}

static json embed(::llama_model* model, const std::string& input) {
    auto ctx_params = llama_context_default_params();
    ctx_params.embeddings = true;
    ctx_params.n_ctx      = 512;
    ctx_params.n_threads  = 4;
    auto* ctx = llama_init_from_model(model, ctx_params);
    if (!ctx) return json::object();

    const auto* vocab = llama_model_get_vocab(model);
    int32_t n_ctx_max = static_cast<int32_t>(llama_n_ctx(ctx));

    std::vector<llama_token> tokens(n_ctx_max);
    int32_t n_tokens = llama_tokenize(vocab, input.c_str(), static_cast<int32_t>(input.size()),
                                       tokens.data(), static_cast<int32_t>(tokens.size()), true, false);
    if (n_tokens < 0) { llama_free(ctx); return json::object(); }
    tokens.resize(n_tokens);

    auto batch = llama_batch_get_one(tokens.data(), static_cast<int32_t>(tokens.size()));
    if (llama_decode(ctx, batch) != 0) { llama_free(ctx); return json::object(); }

    auto* emb = llama_get_embeddings(ctx);
    if (!emb) { llama_free(ctx); return json::object(); }

    int32_t n_embd = static_cast<int32_t>(llama_n_ctx(ctx));
    std::vector<float> data(emb, emb + n_embd);

    llama_free(ctx);
    return json{{"embedding", data}};
}

// ── main ─────────────────────────────────────────────────────────

int main(int argc, char* argv[]) {
    cllama::util::init_logger();
    auto log = cllama::util::get_logger("runner");

    std::string model_path;
    int port = 0;
    std::string chat_template;

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&]() { if (++i >= argc) { log->error("Missing value for {}", a); std::exit(1); } return std::string(argv[i]); };
        if (a == "--model")        model_path = next();
        else if (a == "--port")    port = std::stoi(next());
        else if (a == "--chat-template") chat_template = next();
    }

    if (model_path.empty() || port == 0) {
        log->error("Usage: llama-runner --model <path> --port <port> [--chat-template <name>]");
        return 1;
    }

    llama_backend_init();

    auto mparams = llama_model_default_params();
    auto* model = llama_model_load_from_file(model_path.c_str(), mparams);
    if (!model) {
        log->error("Failed to load model: {}", model_path);
        llama_backend_free();
        return 1;
    }

    std::signal(SIGTERM, signal_handler);
    std::signal(SIGINT, signal_handler);

    httplib::Server svr;
    g_svr = &svr;

    // /health
    svr.Get("/health", [](const httplib::Request&, httplib::Response& res) {
        res.set_content(R"({"status":"ok"})", "application/json");
    });

    // ── shared streaming state ─────────────────────────────────────

    struct TokenStream {
        std::mutex              mtx;
        std::condition_variable cv;
        std::deque<std::string> tokens;
        std::string             text;
        bool                    done    = false;
        bool                    aborted = false;
    };

    auto stream_content_provider = [](std::shared_ptr<TokenStream> ts) {
        return [ts](size_t /*offset*/, httplib::DataSink& sink) -> bool {
            std::unique_lock<std::mutex> lk(ts->mtx);
            ts->cv.wait(lk, [ts]() {
                return !ts->tokens.empty() || ts->done || ts->aborted;
            });

            if (ts->aborted) return false;

            while (!ts->tokens.empty()) {
                std::string line = json{{"token", ts->tokens.front()}}.dump() + "\n";
                ts->tokens.pop_front();
                lk.unlock();
                if (!sink.write(line.data(), line.size())) {
                    lk.lock();
                    ts->aborted = true;
                    ts->cv.notify_all();
                    return false;
                }
                lk.lock();
            }

            if (ts->done) {
                std::string final_line = json{{"done", true}, {"text", ts->text}}.dump() + "\n";
                lk.unlock();
                sink.write(final_line.data(), final_line.size());
                sink.done();
                return false;
            }

            return true;
        };
    };

    auto stream_done_cb = [](std::shared_ptr<TokenStream> ts) {
        return [ts](bool /*success*/) {
            std::lock_guard<std::mutex> lk(ts->mtx);
            ts->aborted = true;
            ts->cv.notify_all();
        };
    };

    auto on_token_push = [](std::shared_ptr<TokenStream> ts) {
        return [ts](const std::string& t) {
            std::lock_guard<std::mutex> lk(ts->mtx);
            if (ts->aborted) return;
            ts->tokens.push_back(t);
            ts->cv.notify_one();
        };
    };

    // /api/completion  —  NDJSON streaming
    svr.Post("/api/completion", [model, stream_content_provider, stream_done_cb](const httplib::Request& req, httplib::Response& res) {
        try {
            auto j      = json::parse(req.body);
            auto prompt = j["prompt"].get<std::string>();
            auto opts   = opts_from_json(j.value("options", json::object()));
            auto ts     = std::make_shared<TokenStream>();

            std::thread([model, prompt, opts, ts]() {
                try {
                    auto on_token = [ts](const std::string& t) {
                        std::lock_guard<std::mutex> lk(ts->mtx);
                        if (ts->aborted) return;
                        ts->tokens.push_back(t);
                        ts->cv.notify_one();
                    };
                    auto text = generate(model, prompt, opts, false, on_token);
                    std::lock_guard<std::mutex> lk(ts->mtx);
                    ts->text = std::move(text);
                    ts->done = true;
                    ts->cv.notify_one();
                } catch (const std::exception& e) {
                    std::lock_guard<std::mutex> lk(ts->mtx);
                    ts->text = e.what();
                    ts->done = true;
                    ts->cv.notify_one();
                }
            }).detach();

            res.set_content_provider("application/x-ndjson",
                                     stream_content_provider(ts),
                                     stream_done_cb(ts));
        } catch (const std::exception& e) {
            res.status = 400;
            res.set_content(json{{"error", e.what()}}.dump(), "application/json");
        }
    });

    // /api/chat  —  NDJSON streaming
    svr.Post("/api/chat", [model, chat_template, stream_content_provider, stream_done_cb](const httplib::Request& req, httplib::Response& res) {
        try {
            auto j    = json::parse(req.body);
            auto msgs = j["messages"];
            auto opts = opts_from_json(j.value("options", json::object()));
            auto tmpl = j.value("chat_template", chat_template);
            auto ts   = std::make_shared<TokenStream>();

            std::thread([model, msgs, opts, tmpl, ts]() {
                try {
                    auto on_token = [ts](const std::string& t) {
                        std::lock_guard<std::mutex> lk(ts->mtx);
                        if (ts->aborted) return;
                        ts->tokens.push_back(t);
                        ts->cv.notify_one();
                    };
                    auto text = chat(model, msgs, opts, tmpl, on_token);
                    std::lock_guard<std::mutex> lk(ts->mtx);
                    ts->text = std::move(text);
                    ts->done = true;
                    ts->cv.notify_one();
                } catch (const std::exception& e) {
                    std::lock_guard<std::mutex> lk(ts->mtx);
                    ts->text = e.what();
                    ts->done = true;
                    ts->cv.notify_one();
                }
            }).detach();

            res.set_content_provider("application/x-ndjson",
                                     stream_content_provider(ts),
                                     stream_done_cb(ts));
        } catch (const std::exception& e) {
            res.status = 400;
            res.set_content(json{{"error", e.what()}}.dump(), "application/json");
        }
    });

    // /api/embeddings
    svr.Post("/api/embeddings", [model](const httplib::Request& req, httplib::Response& res) {
        try {
            auto j    = json::parse(req.body);
            auto input = j["input"].get<std::string>();
            auto data  = embed(model, input);
            res.set_content(data.dump(), "application/json");
        } catch (const std::exception& e) {
            res.status = 400;
            res.set_content(json{{"error", e.what()}}.dump(), "application/json");
        }
    });

    log->info("Runner listening on port {}", port);

    if (!svr.listen("127.0.0.1", port)) {
        log->error("Failed to listen on port {}", port);
        llama_model_free(model);
        llama_backend_free();
        return 1;
    }

    llama_model_free(model);
    llama_backend_free();
    return 0;
}
