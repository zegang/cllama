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
#include <oatpp/macro/component.hpp>
#include <oatpp/Environment.hpp>

#include <oatpp/web/protocol/http/outgoing/StreamingBody.hpp>
#include <oatpp-swagger/Generator.hpp>
#include <oatpp-swagger/Resources.hpp>
#include <oatpp-swagger/Model.hpp>

#include OATPP_CODEGEN_BEGIN(ApiController)

namespace cllama {
namespace api {

// ── I/O pipe ────────────────────────────────────────────────────
// ReadCallback that buffers writes from a background thread;
// oatpp StreamingBody reads from it to produce per-token SSE.

class IOPipe : public oatpp::data::stream::ReadCallback {
public:
    static constexpr size_t MAX_BUFFER = 1024 * 1024; // 1MB watermark ⇒ client likely gone

    oatpp::v_io_size read(void *buffer, ::v_buff_size count, oatpp::async::Action& action) override {
        std::unique_lock<std::mutex> lk(mtx_);
        cv_.wait(lk, [this] { return !buf_.empty() || done_; });
        if (buf_.empty()) return 0;
        auto n = std::min<::v_buff_size>(count, static_cast<::v_buff_size>(buf_.size()));
        std::memcpy(buffer, buf_.data(), static_cast<size_t>(n));
        buf_.erase(buf_.begin(), buf_.begin() + n);
        cv_.notify_one();
        return n;
    }

    /// Returns false if the pipe has been aborted (client likely disconnected).
    bool write(const char* data, size_t len) {
        std::lock_guard<std::mutex> lk(mtx_);
        if (aborted_ || done_) return false;
        buf_.insert(buf_.end(), data, data + len);
        if (buf_.size() > MAX_BUFFER) {
            aborted_ = true;
        }
        cv_.notify_one();
        return !aborted_;
    }

    void close() {
        std::lock_guard<std::mutex> lk(mtx_);
        done_ = true;
        cv_.notify_all();
    }

    bool is_aborted() const { return aborted_; }

private:
    std::mutex mtx_;
    std::condition_variable cv_;
    std::vector<char> buf_;
    bool done_    = false;
    bool aborted_ = false;
};

class OatppController : public oatpp::web::server::api::ApiController {
public:
    OatppController(const std::shared_ptr<ObjectMapper>& mapper,
                    std::shared_ptr<RunnerManager> runner_mgr,
                    const oatpp::Object<oatpp::swagger::oas3::Document>& document,
                    const std::shared_ptr<oatpp::swagger::Resources>& resources,
                    const oatpp::String& apiJson)
        : oatpp::web::server::api::ApiController(mapper)
        , runner_mgr_(std::move(runner_mgr))
        , m_document(document)
        , m_resources(resources)
        , m_apiJson(apiJson)
        , log_(cllama::util::get_logger("oatpp"))
    {
        log_->info("Controller initialized");
    }

    static std::shared_ptr<OatppController> createShared(
        const std::shared_ptr<ObjectMapper>& mapper,
        std::shared_ptr<RunnerManager> runner_mgr,
        const oatpp::Object<oatpp::swagger::oas3::Document>& document,
        const std::shared_ptr<oatpp::swagger::Resources>& resources,
        const oatpp::String& apiJson)
    {
        return std::make_shared<OatppController>(mapper, std::move(runner_mgr),
                                                  document, resources, apiJson);
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
            auto pipe = std::make_shared<IOPipe>();
            auto stream_id = "cmpl-" + std::to_string(now_epoch());
            auto ts = now_epoch();

            std::thread([this, pipe, model, prompt, opts, stream_id, ts]() {
                try {
                    auto result = runner_mgr_->stream_completion(model, prompt, opts,
                        [pipe, stream_id, ts, model](const std::string& token) -> bool {
                            nlohmann::json event;
                            event["id"] = stream_id;
                            event["object"] = "text_completion";
                            event["created"] = ts;
                            event["model"] = model;
                            event["choices"] = nlohmann::json::array({nlohmann::json{
                                {"text", token},
                                {"index", 0},
                                {"logprobs", nullptr},
                                {"finish_reason", nullptr}
                            }});
                            auto sse = "data: " + event.dump() + "\n\n";
                            return pipe->write(sse.data(), sse.size());
                        }
                    );
                    if (!result.success()) {
                        nlohmann::json err;
                        err["error"] = result.error().message;
                        auto sse = "data: " + err.dump() + "\n\n";
                        pipe->write(sse.data(), sse.size());
                    }
                } catch (const std::exception& e) {
                    nlohmann::json err;
                    err["error"] = e.what();
                    auto sse = "data: " + err.dump() + "\n\n";
                    pipe->write(sse.data(), sse.size());
                }
                pipe->write("data: [DONE]\n\n", 14);
                pipe->close();
            }).detach();

            return stream_response(pipe);
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
            auto pipe = std::make_shared<IOPipe>();
            auto stream_id = "chatcmpl-" + std::to_string(now_epoch());
            auto ts = now_epoch();

            std::thread([this, pipe, model, messages, opts, stream_id, ts]() {
                try {
                    // Role event
                    {
                        nlohmann::json role_event;
                        role_event["id"] = stream_id;
                        role_event["object"] = "chat.completion.chunk";
                        role_event["created"] = ts;
                        role_event["model"] = model;
                        role_event["choices"] = nlohmann::json::array({nlohmann::json{
                            {"delta", {{"role", "assistant"}}},
                            {"index", 0},
                            {"finish_reason", nullptr}
                        }});
                        auto sse = "data: " + role_event.dump() + "\n\n";
                        pipe->write(sse.data(), sse.size());
                    }

                    auto result = runner_mgr_->stream_chat(model, messages, opts,
                        [pipe, stream_id, ts, model](const std::string& token) -> bool {
                            nlohmann::json event;
                            event["id"] = stream_id;
                            event["object"] = "chat.completion.chunk";
                            event["created"] = ts;
                            event["model"] = model;
                            event["choices"] = nlohmann::json::array({nlohmann::json{
                                {"delta", {{"content", token}}},
                                {"index", 0},
                                {"finish_reason", nullptr}
                            }});
                            auto sse = "data: " + event.dump() + "\n\n";
                            return pipe->write(sse.data(), sse.size());
                        }
                    );
                    if (!result.success()) {
                        nlohmann::json err;
                        err["error"] = result.error().message;
                        auto sse = "data: " + err.dump() + "\n\n";
                        pipe->write(sse.data(), sse.size());
                    }
                } catch (const std::exception& e) {
                    nlohmann::json err;
                    err["error"] = e.what();
                    auto sse = "data: " + err.dump() + "\n\n";
                    pipe->write(sse.data(), sse.size());
                }
                pipe->write("data: [DONE]\n\n", 14);
                pipe->close();
            }).detach();

            return stream_response(pipe);
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

    // ── GET /openapi.json ────────────────────────────────────

    ENDPOINT("GET", "/openapi.json", serveOpenApi) {
        log_->info("GET /openapi.json");
        return createDtoResponse(Status::CODE_200, m_document);
    }

    // ── GET /docs → redirect or serve ────────────────────────

    ENDPOINT("GET", "/docs", serveDocs,
             REQUEST(std::shared_ptr<oatpp::web::protocol::http::incoming::Request>, request))
    {
        auto& line = request->getStartingLine();
        auto path = line.path.toString();
        if (path && !path->empty() && path->back() == '/') {
            log_->info("GET /docs/  → serving swagger UI");
            return createResponse(Status::CODE_200,
                m_resources->getResourceData("index.html"));
        }
        log_->info("GET /docs  → redirecting to /docs/");
        auto response = createResponse(Status::CODE_302, "");
        response->putHeader("Location", "/docs/");
        return response;
    }

    // ── GET /docs/swagger-initializer.js (must be before {filename}) ─

    ENDPOINT("GET", "/docs/swagger-initializer.js", serveSwaggerInit) {
        std::string ui = m_resources->getResourceData("swagger-initializer.js");
        auto pos = ui.find("%%API.JSON%%");
        if (pos != std::string::npos) {
            ui.replace(pos, 12, m_apiJson);
        }
        auto response = createResponse(Status::CODE_200, ui);
        response->putHeader("Content-Type", "application/javascript");
        return response;
    }

    // ── GET /docs/{filename} ──────────────────────────────────

    ENDPOINT("GET", "/docs/{filename}", serveSwaggerResource,
             PATH(String, filename))
    {
        auto file = m_resources->getResource(filename);
        if (!file) {
            log_->warn("GET /docs/{}  → 404", filename->c_str());
            return createResponse(Status::CODE_404, "");
        }
        auto body = std::make_shared<oatpp::web::protocol::http::outgoing::StreamingBody>(
            file->openInputStream()
        );
        auto resp = OutgoingResponse::createShared(Status::CODE_200, body);
        resp->putHeader("Content-Type", m_resources->getMimeType(filename));
        return resp;
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

    std::shared_ptr<OutgoingResponse> stream_response(std::shared_ptr<IOPipe> pipe) {
        auto body = std::make_shared<oatpp::web::protocol::http::outgoing::StreamingBody>(pipe);
        auto response = OutgoingResponse::createShared(Status::CODE_200, body);
        response->putHeader(Header::CONTENT_TYPE, "text/event-stream");
        response->putHeader("Cache-Control", "no-cache");
        response->putHeader(Header::CONNECTION, "keep-alive");
        return response;
    }

    std::shared_ptr<RunnerManager> runner_mgr_;
    std::shared_ptr<oatpp::swagger::Resources> m_resources;
    oatpp::String m_apiJson;
    std::shared_ptr<spdlog::logger> log_;

public:
    oatpp::Object<oatpp::swagger::oas3::Document> m_document;
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

        // ── OpenAPI / Swagger document generation ────────────────
        oatpp::swagger::DocumentInfo::Builder docBuilder;
        docBuilder
            .setTitle("CLLaMA API")
            .setDescription("Local LLM Inference Server")
            .setVersion("1.0.0")
            .setContactName("CLLaMA Contributors")
            .setLicenseName("MIT License")
            .addServer("http://localhost:" + std::to_string(config_.port),
                        "CLLaMA server (" + config_.host + ")");
        auto doc = docBuilder.build();

        auto resources = oatpp::swagger::Resources::loadResources(OATPP_SWAGGER_RES_PATH);

        auto objectMapper = std::make_shared<oatpp::json::ObjectMapper>();
        auto controller = OatppController::createShared(objectMapper, runner_mgr_,
                                                        oatpp::Object<oatpp::swagger::oas3::Document>(),
                                                        resources, "openapi.json");

        oatpp::web::server::api::Endpoints docEndpoints;
        docEndpoints.append(controller->getEndpoints());

        oatpp::swagger::Generator generator(std::make_shared<oatpp::swagger::Generator::Config>());
        auto document = generator.generateDocument(doc, docEndpoints);
        controller->m_document = document;

        auto router = oatpp::web::server::HttpRouter::createShared();
        router->addController(controller);

        log->info("OpenAPI spec at /openapi.json  Swagger UI at /docs/");

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
