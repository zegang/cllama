#ifndef CLlama_API_OATPP_ROUTER_HPP
#define CLlama_API_OATPP_ROUTER_HPP

#include "cllama/api/router.hpp"
#include <cllama/util/log.hpp>
#include <nlohmann/json.hpp>
#include <thread>
#include <future>

#include <oatpp/web/server/api/ApiController.hpp>
#include <oatpp/web/server/HttpRouter.hpp>
#include <oatpp/web/server/HttpConnectionHandler.hpp>
#include <oatpp/network/Server.hpp>
#include <oatpp/network/tcp/server/ConnectionProvider.hpp>
#include <oatpp/json/ObjectMapper.hpp>
#include <oatpp/macro/codegen.hpp>
#include <oatpp/Environment.hpp>

#include OATPP_CODEGEN_BEGIN(ApiController)

namespace cllama {
namespace api {

class OatppController : public oatpp::web::server::api::ApiController {
public:
    OatppController(const std::shared_ptr<ObjectMapper>& mapper,
                    std::shared_ptr<RunnerManager> runner_mgr)
        : oatpp::web::server::api::ApiController(mapper)
        , runner_mgr_(std::move(runner_mgr))
        , log_(cllama::util::get_logger("oatpp"))
    {
        log_->info("Controller initialized");
    }

    static std::shared_ptr<OatppController> createShared(
        const std::shared_ptr<ObjectMapper>& mapper,
        std::shared_ptr<RunnerManager> runner_mgr)
    {
        return std::make_shared<OatppController>(mapper, std::move(runner_mgr));
    }

    // ── helpers ─────────────────────────────────────────────

    static uint64_t now_epoch() {
        return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
    }

    static std::string escape_json(const std::string& s) {
        std::string out;
        char buf[8];
        for (char c : s) {
            switch (c) {
                case '"':  out += "\\\""; break;
                case '\\': out += "\\\\"; break;
                case '\b': out += "\\b";  break;
                case '\f': out += "\\f";  break;
                case '\n': out += "\\n";  break;
                case '\r': out += "\\r";  break;
                case '\t': out += "\\t";  break;
                default:
                    if (static_cast<unsigned char>(c) < 0x20) {
                        std::snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned char>(c));
                        out += buf;
                    } else {
                        out += c;
                    }
            }
        }
        return out;
    }

    // ── GET / ───────────────────────────────────────────────

    ENDPOINT("GET", "/", root) {
        log_->info("GET /");
        nlohmann::json j;
        j["service"] = "CLLaMA API Server";
        j["version"] = "1.0.0";
        return json_response(Status::CODE_200, j);
    }

    ENDPOINT("GET", "/v1", root_v1) {
        log_->info("GET /v1");
        nlohmann::json j;
        j["service"] = "CLLaMA API Server";
        j["version"] = "1.0.0";
        return json_response(Status::CODE_200, j);
    }
    ENDPOINT("GET", "/v1/", root_v1_) {
        log_->info("GET /v1/");
        nlohmann::json j;
        j["service"] = "CLLaMA API Server";
        j["version"] = "1.0.0";
        return json_response(Status::CODE_200, j);
    }

    // ── GET /health ─────────────────────────────────────────

    ENDPOINT("GET", "/health", health) {
        log_->info("GET /health");
        nlohmann::json j;
        j["status"]  = "healthy";
        j["version"] = "1.0.0";
        return json_response(Status::CODE_200, j);
    }

    // ── GET /v1/models ──────────────────────────────────────

    ENDPOINT("GET", "/v1/models", listModels) {
        log_->info("GET /v1/models");
        auto result = runner_mgr_->list_models();
        nlohmann::json j;
        j["object"] = "list";
        j["data"] = nlohmann::json::array();
        if (result.success()) {
            auto models = result.get();
            log_->info("Found {} model(s)", models.size());
            for (const auto& m : models) {
                nlohmann::json item;
                item["id"]      = m.name;
                item["object"]  = "model";
                item["created"] = now_epoch();
                item["owned_by"] = "cllama";
                j["data"].push_back(item);
            }
        } else {
            log_->warn("list_models failed: {}", result.error().message);
        }
        return json_response(Status::CODE_200, j);
    }

    // ── GET /v1/runners ─────────────────────────────────────

    ENDPOINT("GET", "/v1/runners", listRunners) {
        log_->info("GET /v1/runners");
        auto result = runner_mgr_->list_runners();
        nlohmann::json j;
        j["object"] = "list";
        j["data"] = nlohmann::json::array();
        if (result.success()) {
            auto runners = result.get();
            log_->info("Found {} runner(s)", runners.size());
            for (const auto& r : runners) {
                nlohmann::json item;
                item["name"]        = r.name;
                item["runner_type"]  = to_string(r.runner_type);
                item["port"]        = r.port;
                item["pid"]         = r.pid;
                item["model_path"]  = r.model_path;
                auto tt = std::chrono::system_clock::to_time_t(r.started_at);
                std::ostringstream ss;
                ss << std::put_time(std::gmtime(&tt), "%Y-%m-%dT%H:%M:%SZ");
                item["started_at"]  = ss.str();
                j["data"].push_back(item);
            }
        } else {
            log_->warn("list_runners failed: {}", result.error().message);
        }
        return json_response(Status::CODE_200, j);
    }

    // ── POST /v1/completions ────────────────────────────────

    ENDPOINT("POST", "/v1/completions", completions,
             BODY_STRING(String, bodyStr))
    {
        auto jbody = nlohmann::json::parse(bodyStr->c_str(), nullptr, false);
        if (jbody.is_discarded()) {
            log_->warn("POST /v1/completions — invalid JSON");
            return json_response(Status::CODE_400, nlohmann::json({{"error","invalid JSON"}}));
        }

        auto model  = jbody.value("model", "");
        auto prompt = jbody.value("prompt", "");
        bool stream = jbody.value("stream", false);
        log_->info("POST /v1/completions  model={}  stream={}  prompt_len={}",
                   model, stream, prompt.size());

        CompletionOptions opts;
        if (jbody.contains("max_tokens"))
            opts.max_tokens = jbody["max_tokens"].get<int>();
        if (jbody.contains("temperature"))
            opts.temperature = jbody["temperature"].get<float>();

        if (stream) {
            auto result = runner_mgr_->generate_completion(model, prompt, opts);
            auto stream_id = "cmpl-" + std::to_string(now_epoch());
            auto ts = now_epoch();
            std::string sse_data;
            if (result.success()) {
                auto text = result.get();
                log_->info("Completion stream success  size={}", text.size());
                sse_data = "{"
                    "\"id\":\"" + stream_id + "\","
                    "\"object\":\"text_completion\","
                    "\"created\":" + std::to_string(ts) + ","
                    "\"model\":\"" + model + "\","
                    "\"choices\":[{"
                        "\"text\":\"" + escape_json(text) + "\","
                        "\"index\":0,"
                        "\"logprobs\":null,"
                        "\"finish_reason\":\"stop\""
                    "}]}";
            } else {
                log_->error("Completion stream failed: {}", result.error().message);
                sse_data = "{\"error\":\"" + escape_json(result.error().message) + "\"}";
            }
            auto body = std::string("data: ") + sse_data + "\n\n" + "data: [DONE]\n\n";
            return sse_response(body);
        }

        auto result = runner_mgr_->generate_completion(model, prompt, opts);
        nlohmann::json resp;
        resp["id"]      = "cmpl-" + std::to_string(now_epoch());
        resp["object"]  = "text_completion";
        resp["created"] = now_epoch();
        resp["model"]   = model;
        if (result.success()) {
            auto text = result.get();
            log_->info("Completion success  size={}", text.size());
            resp["choices"] = nlohmann::json::array({nlohmann::json{
                {"text", text},
                {"index", 0},
                {"logprobs", nullptr},
                {"finish_reason", "stop"}
            }});
            resp["usage"] = {{"prompt_tokens", 0}, {"completion_tokens", 0}, {"total_tokens", 0}};
        } else {
            log_->error("Completion failed: {}", result.error().message);
            resp["error"] = nlohmann::json({{"message", result.error().message}, {"type", "server_error"}});
        }
        return json_response(Status::CODE_200, resp);
    }

    // ── POST /v1/chat/completions ───────────────────────────

    ENDPOINT("POST", "/v1/chat/completions", chatCompletions,
             BODY_STRING(String, bodyStr))
    {
        auto jbody = nlohmann::json::parse(bodyStr->c_str(), nullptr, false);
        if (jbody.is_discarded()) {
            log_->warn("POST /v1/chat/completions — invalid JSON");
            return json_response(Status::CODE_400, nlohmann::json({{"error","invalid JSON"}}));
        }

        auto model = jbody.value("model", "");
        bool stream = jbody.value("stream", false);
        log_->info("POST /v1/chat/completions  model={}  stream={}", model, stream);

        std::vector<Message> messages;
        if (jbody.contains("messages")) {
            for (const auto& jm : jbody["messages"]) {
                Message msg;
                msg.role    = jm.value("role", "");
                msg.content = jm.value("content", "");
                messages.push_back(msg);
            }
        }
        log_->info("  messages={}", messages.size());

        CompletionOptions opts;
        if (jbody.contains("temperature"))
            opts.temperature = jbody["temperature"].get<float>();
        if (jbody.contains("max_tokens"))
            opts.max_tokens = jbody["max_tokens"].get<int>();

        if (stream) {
            auto result = runner_mgr_->chat_completion(model, messages, opts);
            auto stream_id = "chatcmpl-" + std::to_string(now_epoch());
            auto ts = now_epoch();
            std::string body_out;
            if (result.success()) {
                auto content = result.get();
                log_->info("Chat stream success  size={}", content.size());
                auto role_event = "{"
                    "\"id\":\"" + stream_id + "\","
                    "\"object\":\"chat.completion.chunk\","
                    "\"created\":" + std::to_string(ts) + ","
                    "\"model\":\"" + model + "\","
                    "\"choices\":[{"
                        "\"delta\":{\"role\":\"assistant\"},"
                        "\"index\":0,"
                        "\"finish_reason\":null"
                    "}]}";
                auto content_event = "{"
                    "\"id\":\"" + stream_id + "\","
                    "\"object\":\"chat.completion.chunk\","
                    "\"created\":" + std::to_string(ts) + ","
                    "\"model\":\"" + model + "\","
                    "\"choices\":[{"
                        "\"delta\":{\"content\":\"" + escape_json(content) + "\"},"
                        "\"index\":0,"
                        "\"finish_reason\":\"stop\""
                    "}]}";
                body_out = "data: " + role_event + "\n\n"
                         + "data: " + content_event + "\n\n"
                         + "data: [DONE]\n\n";
            } else {
                log_->error("Chat stream failed: {}", result.error().message);
                body_out = "data: {\"error\":\"" + escape_json(result.error().message) + "\"}\n\n"
                         + "data: [DONE]\n\n";
            }
            return sse_response(body_out);
        }

        auto result = runner_mgr_->chat_completion(model, messages, opts);
        nlohmann::json resp;
        resp["id"]      = "chatcmpl-" + std::to_string(now_epoch());
        resp["object"]  = "chat.completion";
        resp["created"] = now_epoch();
        resp["model"]   = model;
        if (result.success()) {
            auto content = result.get();
            log_->info("Chat success  size={}", content.size());
            resp["choices"] = nlohmann::json::array({nlohmann::json{
                {"index", 0},
                {"message", {{"role", "assistant"}, {"content", content}}},
                {"logprobs", nullptr},
                {"finish_reason", "stop"}
            }});
            resp["usage"] = {{"prompt_tokens", 0}, {"completion_tokens", 0}, {"total_tokens", 0}};
        } else {
            log_->error("Chat failed: {}", result.error().message);
            resp["error"] = nlohmann::json({{"message", result.error().message}, {"type", "server_error"}});
        }
        return json_response(Status::CODE_200, resp);
    }

    // ── POST /v1/embeddings ─────────────────────────────────

    ENDPOINT("POST", "/v1/embeddings", embeddings,
             BODY_STRING(String, bodyStr))
    {
        auto jbody = nlohmann::json::parse(bodyStr->c_str(), nullptr, false);
        if (jbody.is_discarded()) {
            log_->warn("POST /v1/embeddings — invalid JSON");
            return json_response(Status::CODE_400, nlohmann::json({{"error","invalid JSON"}}));
        }

        auto model = jbody.value("model", "");
        auto input = jbody.value("input", "");
        log_->info("POST /v1/embeddings  model={}  input_len={}", model, input.size());

        auto result = runner_mgr_->generate_embeddings(model, input);
        nlohmann::json resp;
        resp["id"]      = "embd-" + std::to_string(now_epoch());
        resp["object"]  = "list";
        resp["model"]   = model;
        if (result.success()) {
            auto& emb = result.get();
            log_->info("Embedding success  dim={}", emb.data.size());
            nlohmann::json vals = nlohmann::json::array();
            for (auto v : emb.data) vals.push_back(v);
            resp["data"] = nlohmann::json::array({nlohmann::json{
                {"object", "embedding"},
                {"index",  0},
                {"embedding", vals}
            }});
            resp["usage"] = nlohmann::json({{"prompt_tokens", 0}, {"total_tokens", 0}});
        } else {
            log_->error("Embedding failed: {}", result.error().message);
            resp["error"] = nlohmann::json({{"message", result.error().message}, {"type", "server_error"}});
        }
        return json_response(Status::CODE_200, resp);
    }

    // ── POST /v1/run ────────────────────────────────────────

    ENDPOINT("POST", "/v1/run", run,
             BODY_STRING(String, bodyStr))
    {
        auto jbody = nlohmann::json::parse(bodyStr->c_str(), nullptr, false);
        if (jbody.is_discarded()) {
            log_->warn("POST /v1/run — invalid JSON");
            return json_response(Status::CODE_400, nlohmann::json({{"error","invalid JSON"}}));
        }

        auto runner_name = jbody.value("name", "");
        auto runner_bin  = jbody.value("binary", "");
        auto model_name  = jbody.value("model", "");
        auto runner_type = jbody.value("type", "");
        auto model_path  = runner_mgr_->resolve_model_path(model_name);
        log_->info("POST /v1/run  name={}  type={}  model={}  path={}", runner_name, runner_type, model_name, model_path);

        auto result = runner_mgr_->start_runner(runner_name, runner_bin, runner_type, model_path);
        nlohmann::json resp;
        if (result.success()) {
            log_->info("Runner '{}' started successfully", runner_name);
            resp["status"]      = "started";
            resp["name"]        = runner_name;
            resp["runner_type"] = runner_type.empty() ? "cllama" : runner_type;
        } else {
            log_->error("Runner '{}' start failed: {}", runner_name, result.error().message);
            resp["status"] = "error";
            resp["error"]  = result.error().message;
        }
        return json_response(Status::CODE_200, resp);
    }

    // ── POST /v1/stop ───────────────────────────────────────

    ENDPOINT("POST", "/v1/stop", stop,
             BODY_STRING(String, bodyStr))
    {
        auto jbody = nlohmann::json::parse(bodyStr->c_str(), nullptr, false);
        auto runner_name = jbody.value("name", "");
        log_->info("POST /v1/stop  name={}", runner_name);
        runner_mgr_->stop(runner_name);
        log_->info("Runner '{}' stopped", runner_name);
        return json_response(Status::CODE_200, nlohmann::json({{"status","stopped"}, {"name",runner_name}}));
    }

    // ── POST /v1/stop-all ───────────────────────────────────

    ENDPOINT("POST", "/v1/stop-all", stopAll) {
        log_->info("POST /v1/stop-all");
        runner_mgr_->stop_all();
        log_->info("All runners stopped");
        return json_response(Status::CODE_200, nlohmann::json({{"status","stopped_all"}}));
    }

    // ── POST /v1/pull ───────────────────────────────────────

    ENDPOINT("POST", "/v1/pull", pull,
             BODY_STRING(String, bodyStr))
    {
        log_->warn("POST /v1/pull — not supported");
        return json_response(Status::CODE_200,
            nlohmann::json({{"status","error"}, {"error","pull not supported — use Ollama backend"}}));
    }

    // ── POST /v1/delete ─────────────────────────────────────

    ENDPOINT("POST", "/v1/delete", del,
             BODY_STRING(String, bodyStr))
    {
        log_->warn("POST /v1/delete — not supported via API");
        return json_response(Status::CODE_200,
            nlohmann::json({{"status","error"}, {"error","delete not supported via API — use CLI"}}));
    }

private:
    // ── response helpers ────────────────────────────────────

    std::shared_ptr<OutgoingResponse> json_response(const Status& status,
                                                     const nlohmann::json& j) {
        auto response = createResponse(status, j.dump());
        response->putHeader("Content-Type", "application/json");
        return response;
    }

    std::shared_ptr<OutgoingResponse> sse_response(const std::string& body) {
        auto response = createResponse(Status::CODE_200, body);
        response->putHeader("Content-Type", "text/event-stream");
        response->putHeader("Cache-Control", "no-cache");
        response->putHeader("Connection", "keep-alive");
        return response;
    }

    std::shared_ptr<RunnerManager> runner_mgr_;
    std::shared_ptr<spdlog::logger> log_;
};

#include OATPP_CODEGEN_END(ApiController)

class OatppRouter : public Router {
public:
    OatppRouter(const ServerConfig& config, std::shared_ptr<RunnerManager> runner_mgr)
        : Router(config, std::move(runner_mgr))
    {}

    void start() override {
        auto log = cllama::util::get_logger("oatpp");
        log->info("OatppRouter starting on {}:{}", config_.host, config_.port);

        auto objectMapper = std::make_shared<oatpp::json::ObjectMapper>();
        auto controller = OatppController::createShared(objectMapper, runner_mgr_);

        auto router = oatpp::web::server::HttpRouter::createShared();
        router->addController(controller);

        auto connectionHandler = oatpp::web::server::HttpConnectionHandler::createShared(router);
        auto connectionProvider = oatpp::network::tcp::server::ConnectionProvider::createShared(
            {config_.host, static_cast<v_uint16>(config_.port), oatpp::network::Address::IP_4}
        );

        server_ = std::make_shared<oatpp::network::Server>(connectionProvider, connectionHandler);
        thread_ = std::thread([this, log]() {
            log->info("Oatpp server thread started, entering event loop");
            server_->run();
            log->info("Oatpp server thread exited");
        });
        log->info("OatppRouter started");
    }

    void stop() override {
        auto log = cllama::util::get_logger("oatpp");
        log->info("OatppRouter stopping");
        if (server_) { server_->stop(); }
        if (thread_.joinable()) { thread_.join(); }
        log->info("OatppRouter stopped");
    }

private:
    std::shared_ptr<oatpp::network::Server> server_;
    std::thread thread_;
};

} // namespace api
} // namespace cllama

#endif // CLlama_API_OATPP_ROUTER_HPP
