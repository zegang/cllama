#ifndef CLlama_API_CPPREST_ROUTER_HPP
#define CLlama_API_CPPREST_ROUTER_HPP

#include "cllama/api/router.hpp"
#include <nlohmann/json.hpp>
#include <cpprest/http_listener.h>
#include <cpprest/json.h>
#include <cpprest/producerconsumerstream.h>
#include <string>
#include <map>
#include <functional>
#include <mutex>
#include <sstream>
#include <iomanip>
#include <ctime>
#include <cstdio>

using namespace web;
using namespace web::http;
using namespace web::http::experimental::listener;
using namespace web::json;

namespace cllama {
namespace api {

class CppRestRouter : public Router {
public:
    CppRestRouter(const ServerConfig& config, std::shared_ptr<RunnerManager> runner_mgr)
        : Router(config, std::move(runner_mgr)) {}

    void start() override { setup_routes(); listener_.open().wait(); }
    void stop()  override { listener_.close().wait(); }

    const uri& base_uri() const { return base_uri_; }

private:
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
                        std::snprintf(buf, sizeof(buf), "\\u%04x",
                                      static_cast<unsigned char>(c));
                        out += buf;
                    } else {
                        out += c;
                    }
            }
        }
        return out;
    }

    static value make_choice_text(const std::string& text) {
        return value::object(std::vector<std::pair<utility::string_t, value>>{
            {U("text"), value::string(U(text))},
            {U("index"), value::number(0)},
            {U("logprobs"), value::null()},
            {U("finish_reason"), value::string(U("stop"))}
        });
    }

    static value make_choice_message(const std::string& content) {
        return value::object(std::vector<std::pair<utility::string_t, value>>{
            {U("index"), value::number(0)},
            {U("message"), value::object(std::vector<std::pair<utility::string_t, value>>{
                {U("role"), value::string(U("assistant"))},
                {U("content"), value::string(U(content))}
            })},
            {U("logprobs"), value::null()},
            {U("finish_reason"), value::string(U("stop"))}
        });
    }

    void setup_routes() {
        uri_builder ub;
        ub.set_scheme(U("http")).set_host(U(config_.host)).set_port(config_.port);
        base_uri_ = ub.to_uri();
        listener_ = http_listener(base_uri_);
        listener_.support(methods::GET, std::bind(&CppRestRouter::handle_get, this, std::placeholders::_1));
        listener_.support(methods::POST, std::bind(&CppRestRouter::handle_post, this, std::placeholders::_1));
    }

    void handle_get(http_request request) {
        auto path = request.request_uri().path();
        if (path == U("/") || path == U("/v1") || path == U("/v1/"))
            handle_root(request);
        else if (path == U("/v1/models"))
            handle_list_models(request);
        else if (path == U("/v1/runners"))
            handle_list_runners(request);
        else if (path == U("/health"))
            handle_health(request);
        else
            request.reply(status_codes::NotFound, U("Not Found"));
    }

    void handle_post(http_request request) {
        auto path = request.request_uri().path();
        if (path == U("/v1/completions"))
            handle_completions(request);
        else if (path == U("/v1/chat/completions"))
            handle_chat(request);
        else if (path == U("/v1/embeddings"))
            handle_embeddings(request);
        else if (path == U("/v1/run"))
            handle_run(request);
        else if (path == U("/v1/stop"))
            handle_stop(request);
        else if (path == U("/v1/stop-all"))
            handle_stop_all(request);
        else if (path == U("/v1/pull"))
            handle_pull(request);
        else if (path == U("/v1/delete"))
            handle_delete_model(request);
        else
            request.reply(status_codes::NotFound, U("Not Found"));
    }

    void handle_root(http_request request) {
        value response;
        response[U("service")] = value::string(U("CLLaMA API Server"));
        response[U("version")] = value::string(U("1.0.0"));
        request.reply(status_codes::OK, response);
    }

    void handle_list_models(http_request request) {
        auto result = runner_mgr_->list_models();
        value response;
        response[U("object")] = value::string(U("list"));
        if (result.success()) {
            std::vector<value> models;
            for (const auto& m : result.get()) {
                value item;
                item[U("id")]      = value::string(m.name);
                item[U("object")]  = value::string(U("model"));
                item[U("created")] = value::number(now_epoch());
                item[U("owned_by")]= value::string(U("cllama"));
                models.push_back(item);
            }
            response[U("data")] = value::array(models);
        }
        request.reply(status_codes::OK, response);
    }

    void handle_list_runners(http_request request) {
        auto result = runner_mgr_->list_runners();
        value response;
        response[U("object")] = value::string(U("list"));
        if (result.success()) {
            std::vector<value> runners;
            for (const auto& r : result.get()) {
                value item;
                item[U("name")]        = value::string(r.name);
                item[U("runner_type")] = value::string(utility::conversions::to_string_t(to_string(r.runner_type)));
                item[U("port")]        = value::number(r.port);
                item[U("pid")]         = value::number(r.pid);
                item[U("model_path")]  = value::string(r.model_path);
                auto tt = std::chrono::system_clock::to_time_t(r.started_at);
                std::ostringstream ss;
                ss << std::put_time(std::gmtime(&tt), "%Y-%m-%dT%H:%M:%SZ");
                item[U("started_at")]  = value::string(ss.str());
                runners.push_back(item);
            }
            response[U("data")] = value::array(runners);
        }
        request.reply(status_codes::OK, response);
    }

    void handle_health(http_request request) {
        value response;
        response[U("status")]  = value::string(U("healthy"));
        response[U("version")] = value::string(U("1.0.0"));
        request.reply(status_codes::OK, response);
    }

    void handle_completions(http_request request) {
        auto body  = request.extract_json().get();
        auto model = body[U("model")].as_string();
        auto prompt = body[U("prompt")].as_string();
        bool stream = body.has_field(U("stream")) && body[U("stream")].as_bool();

        CompletionOptions opts;
        if (body.has_field(U("max_tokens")))
            opts.max_tokens = body[U("max_tokens")].as_integer();
        if (body.has_field(U("temperature")))
            opts.temperature = static_cast<float>(body[U("temperature")].as_double());

        if (stream) {
            std::string stream_id = "cmpl-" + std::to_string(now_epoch());
            auto ts = now_epoch();
            auto buf = std::make_shared<concurrency::streams::producer_consumer_buffer<uint8_t>>();
            auto body_stream = buf->create_istream();

            http_response resp(status_codes::OK);
            resp.set_body(body_stream, U("text/event-stream"));
            resp.headers().add(U("Cache-Control"), U("no-cache"));
            resp.headers().add(U("Connection"), U("keep-alive"));

            request.reply(resp);

            auto on_token = [buf, stream_id, ts, model](const std::string& token) -> bool {
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
                auto sse = std::string("data: ") + event.dump() + "\n\n";
                try {
                    buf->putn_nocopy(reinterpret_cast<const uint8_t*>(sse.data()), sse.size()).wait();
                    return true;
                } catch (...) {
                    return false;
                }
            };

            auto result = runner_mgr_->stream_completion(model, prompt, opts, on_token);
            if (!result.success()) {
                auto err = "data: {\"error\":\"" + escape_json(result.error().message) + "\"}\n\n";
                buf->putn_nocopy(reinterpret_cast<const uint8_t*>(err.data()), err.size()).wait();
            }
            auto done = std::string("data: [DONE]\n\n");
            buf->putn_nocopy(reinterpret_cast<const uint8_t*>(done.data()), done.size()).wait();
            buf->close().wait();
        } else {
            auto result = runner_mgr_->generate_completion(model, prompt, opts);
            auto id = std::string("cmpl-") + std::to_string(now_epoch());
            value response;
            response[U("id")]      = value::string(U(id));
            response[U("object")]  = value::string(U("text_completion"));
            response[U("created")] = value::number(now_epoch());
            response[U("model")]   = value::string(U(model));
            if (result.success()) {
                response[U("choices")] = value::array({make_choice_text(result.get())});
                response[U("usage")] = value::object(std::vector<std::pair<utility::string_t, value>>{
                    {U("prompt_tokens"), value::number(0)},
                    {U("completion_tokens"), value::number(0)},
                    {U("total_tokens"), value::number(0)}
                });
            } else {
                response[U("error")] = value::object(std::vector<std::pair<utility::string_t, value>>{
                    {U("message"), value::string(U(result.error().message))},
                    {U("type"), value::string(U("server_error"))}
                });
            }
            request.reply(status_codes::OK, response);
        }
    }

    void handle_chat(http_request request) {
        auto body  = request.extract_json().get();
        auto model = body[U("model")].as_string();
        bool stream = body.has_field(U("stream")) && body[U("stream")].as_bool();

        std::vector<Message> messages;
        for (const auto& jm : body[U("messages")].as_array()) {
            Message msg;
            msg.role    = jm.at(U("role")).as_string();
            msg.content = jm.at(U("content")).as_string();
            messages.push_back(msg);
        }

        CompletionOptions opts;
        if (body.has_field(U("temperature")))
            opts.temperature = static_cast<float>(body[U("temperature")].as_double());
        if (body.has_field(U("max_tokens")))
            opts.max_tokens = body[U("max_tokens")].as_integer();

        if (stream) {
            std::string stream_id = "chatcmpl-" + std::to_string(now_epoch());
            auto ts = now_epoch();
            auto buf = std::make_shared<concurrency::streams::producer_consumer_buffer<uint8_t>>();
            auto body_stream = buf->create_istream();

            http_response resp(status_codes::OK);
            resp.set_body(body_stream, U("text/event-stream"));
            resp.headers().add(U("Cache-Control"), U("no-cache"));
            resp.headers().add(U("Connection"), U("keep-alive"));

            request.reply(resp);

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
                auto role_sse = std::string("data: ") + role_event.dump() + "\n\n";
                buf->putn_nocopy(reinterpret_cast<const uint8_t*>(role_sse.data()), role_sse.size()).wait();
            }

            auto on_token = [buf, stream_id, ts, model](const std::string& token) -> bool {
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
                auto sse = std::string("data: ") + event.dump() + "\n\n";
                try {
                    buf->putn_nocopy(reinterpret_cast<const uint8_t*>(sse.data()), sse.size()).wait();
                    return true;
                } catch (...) {
                    return false;
                }
            };

            auto result = runner_mgr_->stream_chat(model, messages, opts, on_token);
            if (!result.success()) {
                auto err = "data: {\"error\":\"" + escape_json(result.error().message) + "\"}\n\n";
                buf->putn_nocopy(reinterpret_cast<const uint8_t*>(err.data()), err.size()).wait();
            }
            auto done = std::string("data: [DONE]\n\n");
            buf->putn_nocopy(reinterpret_cast<const uint8_t*>(done.data()), done.size()).wait();
            buf->close().wait();
        } else {
            auto result = runner_mgr_->chat_completion(model, messages, opts);
            auto id = std::string("chatcmpl-") + std::to_string(now_epoch());
            value response;
            response[U("id")]      = value::string(U(id));
            response[U("object")]  = value::string(U("chat.completion"));
            response[U("created")] = value::number(now_epoch());
            response[U("model")]   = value::string(U(model));
            if (result.success()) {
                response[U("choices")] = value::array({make_choice_message(result.get())});
                response[U("usage")] = value::object(std::vector<std::pair<utility::string_t, value>>{
                    {U("prompt_tokens"), value::number(0)},
                    {U("completion_tokens"), value::number(0)},
                    {U("total_tokens"), value::number(0)}
                });
            } else {
                response[U("error")] = value::object(std::vector<std::pair<utility::string_t, value>>{
                    {U("message"), value::string(U(result.error().message))},
                    {U("type"), value::string(U("server_error"))}
                });
            }
            request.reply(status_codes::OK, response);
        }
    }

    void handle_embeddings(http_request request) {
        auto body  = request.extract_json().get();
        auto model = body[U("model")].as_string();
        auto input = body[U("input")].as_string();

        auto result = runner_mgr_->generate_embeddings(model, input);
        auto id = std::string("embd-") + std::to_string(now_epoch());
        value response;
        response[U("id")]      = value::string(U(id));
        response[U("object")]  = value::string(U("list"));
        response[U("model")]   = value::string(U(model));
        if (result.success()) {
            auto& emb = result.get();
            std::vector<value> vals;
            for (auto v : emb.data) vals.push_back(value::number(v));
            response[U("data")] = value::array(std::vector<value>{
                value::object(std::vector<std::pair<utility::string_t, value>>{
                    {U("object"), value::string(U("embedding"))},
                    {U("index"), value::number(0)},
                    {U("embedding"), value::array(vals)}
                })
            });
            response[U("usage")] = value::object(std::vector<std::pair<utility::string_t, value>>{
                {U("prompt_tokens"), value::number(0)},
                {U("total_tokens"), value::number(0)}
            });
        } else {
            response[U("error")] = value::object(std::vector<std::pair<utility::string_t, value>>{
                {U("message"), value::string(U(result.error().message))},
                {U("type"), value::string(U("server_error"))}
            });
        }
        request.reply(status_codes::OK, response);
    }

    void handle_run(http_request request) {
        auto body = request.extract_json().get();
        auto runner_name = body[U("name")].as_string();
        auto runner_bin  = body[U("binary")].as_string();
        auto model_name  = body[U("model")].as_string();
        auto runner_type = body.has_field(U("type")) ? body[U("type")].as_string() : utility::string_t();
        auto model_path  = runner_mgr_->resolve_model_path(model_name);

        auto result = runner_mgr_->start_runner(runner_name, runner_bin, runner_type, model_path);
        value response;
        if (result.success()) {
            response[U("status")]      = value::string(U("started"));
            response[U("name")]        = value::string(runner_name);
            response[U("runner_type")] = value::string(runner_type.empty() ? U("cllama") : runner_type);
        } else {
            response[U("status")] = value::string(U("error"));
            response[U("error")]  = value::string(result.error().message);
        }
        request.reply(status_codes::OK, response);
    }

    void handle_stop(http_request request) {
        auto body = request.extract_json().get();
        auto runner_name = body[U("name")].as_string();
        runner_mgr_->stop(runner_name);
        value response;
        response[U("status")] = value::string(U("stopped"));
        response[U("name")]   = value::string(runner_name);
        request.reply(status_codes::OK, response);
    }

    void handle_stop_all(http_request request) {
        runner_mgr_->stop_all();
        value response;
        response[U("status")] = value::string(U("stopped_all"));
        request.reply(status_codes::OK, response);
    }

    void handle_pull(http_request request) {
        value response;
        response[U("status")] = value::string(U("error"));
        response[U("error")]  = value::string(U("pull not supported — use Ollama backend"));
        request.reply(status_codes::OK, response);
    }

    void handle_delete_model(http_request request) {
        value response;
        response[U("status")] = value::string(U("error"));
        response[U("error")]  = value::string(U("delete not supported via API — use CLI"));
        request.reply(status_codes::OK, response);
    }

    uri base_uri_;
    http_listener listener_;
};

} // namespace api
} // namespace cllama

#endif // CLlama_API_CPPREST_ROUTER_HPP
