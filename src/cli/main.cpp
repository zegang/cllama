#include <iostream>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <thread>
#include <csignal>
#include <iomanip>
#include <cctype>
#include <cstring>
#include <cstdlib>
#include <chrono>
#include <httplib.h>
#include <nlohmann/json.hpp>
#include <cllama/api/router.hpp>
#include <cllama/api/cpprest_router.hpp>
#include <cllama/api/oatpp_router.hpp>
#include <cllama/api/config.hpp>
#include <cllama/core/runner_mgr.hpp>
#include <cllama/util/cflag.hpp>
#include <cllama/util/log.hpp>
#include <spdlog/spdlog.h>
#include <oatpp/Environment.hpp>

#include <cllama/platform/os.hpp>

using namespace cllama;
using namespace cllama::api;
namespace flag = cllama::util;
namespace plat = cllama::platform;

static std::shared_ptr<RunnerManager> g_runner_mgr;
static std::unique_ptr<Router>        g_router;
static volatile std::sig_atomic_t     g_running = 1;
static std::string                    g_pidfile;

static void signal_handler(int) { g_running = 0; }

static void cleanup_pidfile() {
    if (!g_pidfile.empty()) {
        std::remove(g_pidfile.c_str());
        g_pidfile.clear();
    }
}

static int daemonize(const std::string& pidfile) {
    int ret = plat::daemonize(pidfile);
    if (ret == 0 && !pidfile.empty())
        g_pidfile = pidfile;
    if (ret == 0)
        std::atexit(cleanup_pidfile);
    return ret;
}

static std::string runner_binary_path(const char* argv0) {
    std::string path = argv0;
    auto slash = path.find_last_of("/\\");
    std::string dir = (slash != std::string::npos) ? path.substr(0, slash) : ".";
    return dir + "/cllama_runner";
}

// ── serve list ─────────────────────────────────────────────────

static int cmd_serve_list() {
    auto procs = plat::list_processes();
    if (procs.empty()) {
        auto test = plat::list_processes();
        (void)test;
    }

    bool found = false;
    for (const auto& p : procs) {
        if (p.comm.find("cllama") == std::string::npos) continue;

        std::string cmdline = plat::get_process_cmdline(p.pid);
        if (cmdline.find("cllama serve") == std::string::npos) continue;

        // skip non-server subcommand invocations (list, stop, help)
        auto sp = cmdline.find("cllama serve") + 13;
        while (sp < cmdline.size() && cmdline[sp] == ' ') ++sp;
        if (sp < cmdline.size()) {
            std::string rest;
            while (sp < cmdline.size() && cmdline[sp] != ' ') rest += cmdline[sp++];
            if (rest == "list" || rest == "stop" || rest == "--help" || rest == "-h")
                continue;
        }

        std::string host = "0.0.0.0", models = "./models", name;
        int port = 8080;
        bool daemon = false;

        std::istringstream iss(cmdline);
        std::vector<std::string> args;
        std::string arg;
        while (iss >> arg) args.push_back(arg);
        for (size_t i = 0; i < args.size(); ++i) {
            if (args[i] == "--host" || args[i] == "-H") { if (i + 1 < args.size()) host = args[++i]; }
            else if (args[i] == "--port" || args[i] == "-p") { if (i + 1 < args.size()) port = std::stoi(args[++i]); }
            else if (args[i] == "--models-folder") { if (i + 1 < args.size()) models = args[++i]; }
            else if (args[i] == "--name" || args[i] == "-n") { if (i + 1 < args.size()) name = args[++i]; }
            else if (args[i] == "--daemon" || args[i] == "-d") { daemon = true; }
        }

        // read started_at from companion info file
        std::string started_at = "-";
        {
            std::string info_path = plat::temp_directory() + "cllama-serve-" + (name.empty() ? std::to_string(p.pid) : name) + ".info";
            std::ifstream inf(info_path);
            if (inf) std::getline(inf, started_at);
        }

        if (!found) {
            std::cout << std::left
                      << std::setw(8)  << "PID"
                      << std::setw(22) << "HOST:PORT"
                      << std::setw(18) << "NAME"
                      << std::setw(36) << "MODELS"
                      << std::setw(8)  << "DAEMON"
                      << "STARTED_AT\n"
                      << std::string(106, '-') << "\n";
            found = true;
        }
        std::cout << std::left
                  << std::setw(8)  << p.pid
                  << std::setw(22) << (host + ":" + std::to_string(port))
                  << std::setw(18) << (name.empty() ? "-" : name)
                  << std::setw(36) << models
                  << std::setw(8)  << (daemon ? "yes" : "no")
                  << started_at << "\n";
    }

    if (!found) std::cout << "No running serve instances.\n";
    return 0;
}

// ── runner list (API via server) ──────────────────────────────

static int cmd_runner_list(const std::string& server_url) {
    auto log = cllama::util::get_logger("cli");
    httplib::Client cli(server_url);
    auto res = cli.Get("/v1/runners");
    if (!res) {
        log->error("Failed to connect to server at {}", server_url);
        return 1;
    }
    try {
        auto j = nlohmann::json::parse(res->body);
        auto& data = j["data"];
        if (data.empty()) {
            std::cout << "No active runners.\n";
        } else {
            std::cout << std::left
                      << std::setw(8)  << "PID"
                      << std::setw(8)  << "PORT"
                      << std::setw(12) << "TYPE"
                      << std::setw(20) << "NAME"
                      << std::setw(28) << "MODEL"
                      << "STARTED_AT\n"
                      << std::string(96, '-') << "\n";
            for (const auto& r : data) {
                std::string model_path = r.value("model_path", "");
                auto slash = model_path.find_last_of("/\\");
                std::string model_name = (slash != std::string::npos) ? model_path.substr(slash + 1) : model_path;
                std::cout << std::left
                          << std::setw(8)  << std::to_string(r.value("pid", 0))
                          << std::setw(8)  << std::to_string(r.value("port", 0))
                          << std::setw(12) << r.value("runner_type", "?")
                          << std::setw(20) << r.value("name", "?")
                          << std::setw(28) << model_name
                          << r.value("started_at", "?") << "\n";
            }
        }
    } catch (...) {
        std::cout << res->body << "\n";
    }
    return 0;
}

// ── runner stop (API via server) ──────────────────────────────

static int cmd_runner_stop(const std::string& server_url, const std::string& name) {
    auto log = cllama::util::get_logger("cli");
    httplib::Client cli(server_url);

    if (name == "all") {
        auto res = cli.Post("/v1/stop-all", "{}", "application/json");
        if (!res) {
            log->error("Failed to connect to server at {}", server_url);
            return 1;
        }
        try {
            auto j = nlohmann::json::parse(res->body);
            std::cout << "All runners stopped: " << j.value("status", "?") << "\n";
        } catch (...) {
            std::cout << res->body << "\n";
        }
        return 0;
    }

    nlohmann::json body;
    body["name"] = name;
    auto res = cli.Post("/v1/stop", body.dump(), "application/json");
    if (!res) {
        log->error("Failed to connect to server at {}", server_url);
        return 1;
    }
    try {
        auto j = nlohmann::json::parse(res->body);
        std::cout << "Runner stopped: " << j.value("name", name) << "\n";
    } catch (...) {
        std::cout << res->body << "\n";
    }
    return 0;
}

// ── runner start (API via server) ─────────────────────────────

static int cmd_runner_start(const std::string& server_url,
                            const std::string& runner_name,
                            const std::string& binary,
                            const std::string& runner_type,
                            const std::string& model)
{
    auto log = cllama::util::get_logger("cli");
    httplib::Client cli(server_url);

    nlohmann::json body;
    body["name"]   = runner_name.empty() ? model : runner_name;
    body["binary"] = binary;
    body["model"]  = model;
    if (!runner_type.empty())
        body["type"] = runner_type;

    auto res = cli.Post("/v1/run", body.dump(), "application/json");
    if (!res) {
        log->error("Failed to connect to server at {}", server_url);
        return 1;
    }
    if (res->status != 200) {
        log->error("Server returned HTTP {}: {}", res->status, res->body);
        return 1;
    }
    try {
        auto j = nlohmann::json::parse(res->body);
        if (j.value("status", "") == "error") {
            log->error("Error: {}", j.value("error", "unknown"));
            return 1;
        }
        std::cout << "Runner started: " << j.value("name", runner_name.empty() ? model : runner_name) << "\n";
    } catch (...) {
        std::cout << res->body << "\n";
    }
    return 0;
}

// ── serve stop (process kill) ─────────────────────────────────

/// Resolve a serve name to a PID by reading the pidfile.
static int resolve_serve_name(const std::string& name) {
    std::string pf = plat::temp_directory() + "cllama-serve-" + name + ".pid";
    std::ifstream ifs(pf);
    if (!ifs) return -1;
    int pid{};
    ifs >> pid;
    if (!ifs || pid <= 0) return -1;
    if (!plat::process_is_alive(pid)) {
        std::remove(pf.c_str());
        return -1;
    }
    return pid;
}

static void stop_process(int pid, const std::string& label) {
    // try to clean up pidfile from cmdline
    std::string cmdline = plat::get_process_cmdline(pid);
    if (!cmdline.empty()) {
        auto pp = cmdline.find("--pidfile ");
        if (pp != std::string::npos) {
            auto start = pp + 10;
            auto end = cmdline.find(' ', start);
            std::string pf = cmdline.substr(start, end - start);
            if (!pf.empty()) std::remove(pf.c_str());
        }
        pp = cmdline.find("--name ");
        if (pp != std::string::npos) {
            auto start = pp + 7;
            auto end = cmdline.find(' ', start);
            std::string nm = cmdline.substr(start, end - start);
            if (!nm.empty())
                std::remove((plat::temp_directory() + "cllama-serve-" + nm + ".pid").c_str());
        }
    }

    std::cout << "Stopping serve (" << label << ")...\n";
    plat::kill_process(pid, plat::SIG_TERM);
    for (int i = 0; i < 50; ++i) {
        if (!plat::process_is_alive(pid)) { std::cout << "Stopped.\n"; return; }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    std::cout << "Not responding, sending SIGKILL...\n";
    plat::kill_process(pid, plat::SIG_KILL);
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    if (!plat::process_is_alive(pid)) { std::cout << "Killed.\n"; return; }
    std::cerr << "error: failed to stop PID " << pid << "\n";
}

static int cmd_serve_stop_all() {
    auto procs = plat::list_processes();
    int stopped = 0;
    for (const auto& p : procs) {
        if (p.comm.find("cllama") == std::string::npos) continue;
        std::string cmdline = plat::get_process_cmdline(p.pid);
        if (cmdline.find("cllama serve") == std::string::npos) continue;
        auto sp = cmdline.find("cllama serve") + 13;
        while (sp < cmdline.size() && cmdline[sp] == ' ') ++sp;
        if (sp < cmdline.size()) {
            std::string rest;
            while (sp < cmdline.size() && cmdline[sp] != ' ') rest += cmdline[sp++];
            if (rest == "list" || rest == "stop" || rest == "--help" || rest == "-h")
                continue;
        }
        stop_process(p.pid, std::to_string(p.pid));
        ++stopped;
    }
    if (stopped == 0)
        std::cout << "No running serve instances found.\n";
    else
        std::cout << "Stopped " << stopped << " serve instance(s).\n";
    return 0;
}

static int cmd_serve_stop(const std::string& target) {
    if (target.empty()) {
        std::cerr << "error: specify a serve name or PID, or \"all\"\n";
        return 1;
    }
    if (target == "all")
        return cmd_serve_stop_all();

    bool is_pid = true;
    for (char c : target)
        if (!std::isdigit(c)) { is_pid = false; break; }

    int pid{};
    std::string label;

    if (is_pid) {
        pid = std::stoi(target);
        label = "PID " + std::to_string(pid);
        if (!plat::process_is_alive(pid)) {
            std::cerr << "error: no process with PID " << pid << "\n";
            return 1;
        }
    } else {
        pid = resolve_serve_name(target);
        if (pid < 0) {
            std::cerr << "error: no running serve instance named \"" << target << "\"\n";
            return 1;
        }
        label = "\"" + target + "\" (PID " + std::to_string(pid) + ")";
    }

    stop_process(pid, label);
    return 0;
}

// ── runner start (server daemon) ──────────────────────────────

static int cmd_serve(const std::string& serve_name,
                     const std::string& host, int port,
                     const std::string& models_folder,
                     const std::string& runner_bin,
                     const std::string& impl,
                     bool daemon_flag,
                     const std::string& pidfile,
                     const std::string& log_file)
{
    // auto-derive log file: named → cllama_server_<name>.log, unnamed → cllama_server_<pid>.log
    std::string effective_log = log_file;
    if (effective_log.empty()) {
        effective_log = serve_name.empty()
            ? "cllama_server_" + std::to_string(plat::get_current_pid()) + ".log"
            : "cllama_server_" + serve_name + ".log";
    }
    cllama::util::set_log_file(effective_log);

    auto log = cllama::util::get_logger("serve");

    // auto-derive pidfile from name if not explicitly set
    std::string effective_pidfile = pidfile;
    if (!serve_name.empty() && effective_pidfile.empty())
        effective_pidfile = plat::temp_directory() + "cllama-serve-" + serve_name + ".pid";

    if (daemon_flag) {
        log->info("Daemonizing...");
        if (daemonize(effective_pidfile) != 0) {
            log->error("Failed to daemonize");
            return 1;
        }
    }

    // Write PID file even without --daemon if --pidfile was given
    if (!effective_pidfile.empty() && !daemon_flag) {
        std::ofstream pf(effective_pidfile);
        if (pf) {
            pf << plat::get_current_pid() << "\n";
            pf.close();
            g_pidfile = effective_pidfile;
        }
        std::atexit(cleanup_pidfile);
    }

    // Record started_at to companion file for serve list display
    {
        auto now = std::chrono::system_clock::now();
        auto tt  = std::chrono::system_clock::to_time_t(now);
        std::ostringstream ss;
        ss << std::put_time(std::gmtime(&tt), "%Y-%m-%dT%H:%M:%SZ");
        std::string info_path;
        if (!serve_name.empty())
            info_path = plat::temp_directory() + "cllama-serve-" + serve_name + ".info";
        else
            info_path = plat::temp_directory() + "cllama-serve-" + std::to_string(plat::get_current_pid()) + ".info";
        std::ofstream inf(info_path);
        if (inf) { inf << ss.str() << "\n"; inf.close(); }
    }

    oatpp::Environment::init();

    g_runner_mgr = std::make_shared<RunnerManager>(runner_bin, models_folder);

    ServerConfig cfg = ServerConfig::apply_env_overrides({});
    cfg.host = host;
    cfg.port = port;

    if (impl == "cpprest") {
        g_router = std::make_unique<CppRestRouter>(cfg, g_runner_mgr);
    } else {
        g_router = std::make_unique<OatppRouter>(cfg, g_runner_mgr);
    }

    log->info("Router: {}", impl);
    g_router->start();
    log->info("Server listening on http://{}:{}", host, port);

    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);
    while (g_running) std::this_thread::sleep_for(std::chrono::milliseconds(200));

    g_router->stop();
    g_runner_mgr->stop_all();
    log->info("Server stopped.");

    oatpp::Environment::destroy();
    return 0;
}

// ── model list ───────────────────────────────────────────────────

static int cmd_model_list(const std::string& server_url) {
    auto log = cllama::util::get_logger("cli");
    httplib::Client cli(server_url);
    auto res = cli.Get("/v1/models");
    if (!res) {
        log->error("Failed to connect to server at {}", server_url);
        return 1;
    }
    try {
        auto j = nlohmann::json::parse(res->body);
        auto& data = j["data"];
        if (data.empty()) {
            std::cout << "No models found.\n";
        } else {
            for (const auto& m : data)
                std::cout << m["id"] << "\n";
        }
    } catch (...) {
        std::cout << res->body << "\n";
    }
    return 0;
}

static int cmd_model_pull(const std::string& server_url, const std::string& name) {
    auto log = cllama::util::get_logger("cli");
    httplib::Client cli(server_url);
    nlohmann::json body;
    body["model"] = name;
    auto res = cli.Post("/v1/pull", body.dump(), "application/json");
    if (!res) {
        log->error("Failed to connect to server at {}", server_url);
        return 1;
    }
    try {
        auto j = nlohmann::json::parse(res->body);
        if (j.value("status", "") == "error")
            log->error("Error: {}", j.value("error", "unknown"));
        else
            std::cout << "Pulled " << name << "\n";
    } catch (...) {
        std::cout << res->body << "\n";
    }
    return 0;
}

static int cmd_model_remove(const std::string& server_url, const std::string& name) {
    auto log = cllama::util::get_logger("cli");
    httplib::Client cli(server_url);
    nlohmann::json body;
    body["model"] = name;
    auto res = cli.Post("/v1/delete", body.dump(), "application/json");
    if (!res) {
        log->error("Failed to connect to server at {}", server_url);
        return 1;
    }
    try {
        auto j = nlohmann::json::parse(res->body);
        if (j.value("status", "") == "error")
            log->error("Error: {}", j.value("error", "unknown"));
        else
            std::cout << "Removed " << name << "\n";
    } catch (...) {
        std::cout << res->body << "\n";
    }
    return 0;
}

static int cmd_model_show(const std::string& server_url, const std::string& name) {
    auto log = cllama::util::get_logger("cli");
    httplib::Client cli(server_url);
    auto res = cli.Get("/v1/models");
    if (!res) {
        log->error("Failed to connect to server at {}", server_url);
        return 1;
    }
    try {
        auto j = nlohmann::json::parse(res->body);
        auto& data = j["data"];
        for (const auto& m : data) {
            if (m["id"] == name) {
                std::cout << m.dump(2) << "\n";
                return 0;
            }
        }
        log->warn("Model not found: {}", name);
    } catch (...) {
        std::cout << res->body << "\n";
    }
    return 0;
}

static int cmd_runner_chat(const std::string& server_url, const std::string& name,
                           int max_tokens, float temp, float top_p, int top_k)
{
    auto log = cllama::util::get_logger("cli");
    httplib::Client cli(server_url);

    std::cout << "Chat with " << name << " started. Type /bye to exit.\n\n";

    std::vector<nlohmann::json> history;
    std::string line;

    while (true) {
        std::cout << "You: " << std::flush;
        if (!std::getline(std::cin, line)) break;
        if (line.empty()) continue;
        if (line == "/bye" || line == "/exit") break;
        if (line == "/reset") {
            history.clear();
            std::cout << "Conversation reset.\n";
            continue;
        }
        if (line == "/help") {
            std::cout << "Commands: /bye /exit /reset /help\n";
            continue;
        }

        history.push_back({{"role", "user"}, {"content", line}});

        nlohmann::json msgs = nlohmann::json::array();
        for (const auto& m : history) msgs.push_back(m);

        nlohmann::json body;
        body["model"]       = name;
        body["messages"]    = msgs;
        body["max_tokens"]  = max_tokens;
        body["temperature"] = temp;
        body["top_p"]       = top_p;
        body["top_k"]       = top_k;

        auto res = cli.Post("/v1/chat/completions", body.dump(), "application/json");
        if (!res) {
            log->error("Failed to connect to server at {}", server_url);
            history.pop_back();
            continue;
        }
        if (res->status != 200) {
            log->error("Server error: {}", res->body);
            history.pop_back();
            continue;
        }

        std::string reply;
        try {
            auto j = nlohmann::json::parse(res->body);
            if (j.contains("choices") && !j["choices"].empty()) {
                reply = j["choices"][0]["message"]["content"];
            } else if (j.contains("error")) {
                log->error("Error: {}", j["error"].dump());
                history.pop_back();
                continue;
            }
        } catch (...) {
            log->error("Failed to parse response.");
            history.pop_back();
            continue;
        }

        for (const auto& tok : { "<|im_start|>", "<|im_end|>", "<s>", "</s>", "[INST]", "[/INST]" })
            for (size_t pos; (pos = reply.find(tok)) != std::string::npos; )
                reply.erase(pos, std::strlen(tok));

        history.push_back({{"role", "assistant"}, {"content", reply}});
        std::cout << "AI: " << reply << "\n\n";
    }

    std::cout << "\nGoodbye!\n";
    return 0;
}

// ── main ────────────────────────────────────────────────────────

int main(int argc, char* argv[]) {
    cllama::util::init_logger();

    std::string models_folder = "./models";
    std::string server_url    = "http://localhost:8080";
    std::string model_name;
    std::string runner_name;
    std::string runner_bin;
    std::string runner_type;
    std::string router_impl  = "oatpp";
    std::string host  = "0.0.0.0";
    int port          = 8080;
    bool daemon_flag  = false;
    std::string pidfile;

    std::string serve_name;
    std::string serve_stop_target;
    std::string log_file;
    int   chat_max_tokens = 512;
    float chat_temp       = 0.7f;
    float chat_top_p      = 0.9f;
    int   chat_top_k      = 40;

    flag::App app("cllama", "CLLaMA — local LLM inference server");

    auto& global = app.add_command("_global", "")
        .add_option("--models-folder", &models_folder, "./models", "Path to local .gguf models");
    app.set_default_command(global);

    // ── serve ────────────────────────────────────────────────
    auto& serve_cmd = app.add_command("serve", "Manage API server instances");
    serve_cmd.add_subcommand("start", "Start the REST API server")
        .add_option("--name,-n", &serve_name, "", "Server instance name (auto-derives pidfile)")
        .add_option("--log-file", &log_file, "", "Log file path (default: cllama_server_<name>.log)")
        .add_option("--host,-H", &host, "0.0.0.0", "Host address")
        .add_option("--port,-p", &port, 8080,      "Port number")
        .add_option("--impl,--router", &router_impl, "oatpp", "Router impl: cpprest or oatpp")
        .add_option("--models-folder", &models_folder, "./models", "Path to local .gguf models", true)
        .add_flag("--daemon,-d", &daemon_flag, "Run as a background daemon")
        .add_option("--pidfile", &pidfile, "", "PID file path (default: no pidfile)");
    serve_cmd.add_subcommand("list", "List running serve instances");
    serve_cmd.add_subcommand("stop", "Stop a serve instance by name or PID")
        .add_argument("name", &serve_stop_target, "Server name (from --name) or PID");

    // ── runner ───────────────────────────────────────────────
    auto& runner_cmd = app.add_command("runner", "Manage backend model runners");
    runner_cmd.add_subcommand("start", "Start a model runner via the server")
        .add_option("--server",  &server_url, "http://localhost:8080", "Server URL")
        .add_option("--name,-n", &runner_name,"",                      "Runner name (default: model name)")
        .add_option("--binary",  &runner_bin, "",                      "Runner binary path")
        .add_option("--type",    &runner_type, "cllama",               "Runner type (default: cllama)")
        .add_argument("model",   &model_name,                          "Model name");
    runner_cmd.add_subcommand("list", "List active runners via the server")
        .add_option("--server", &server_url, "http://localhost:8080", "Server URL");
    runner_cmd.add_subcommand("stop", "Stop a runner via the server")
        .add_option("--server", &server_url, "http://localhost:8080", "Server URL")
        .add_argument("name",   &runner_name,                         "Runner name");
    runner_cmd.add_subcommand("chat", "Interactive chat with a model")
        .add_option("--server",     &server_url,   "http://localhost:8080", "Server URL")
        .add_option("--temp",       &chat_temp,    0.7f,   "Temperature")
        .add_option("--top-p",      &chat_top_p,   0.9f,   "Top-p sampling")
        .add_option("--top-k",      &chat_top_k,   40,     "Top-k sampling")
        .add_option("--max-tokens", &chat_max_tokens, 512, "Max tokens per response")
        .add_argument("name", &runner_name, "Model name");

    // ── model ────────────────────────────────────────────────
    auto& model_cmd = app.add_command("model", "Manage models");
    model_cmd.add_subcommand("list", "List available models")
        .add_option("--server", &server_url, "http://localhost:8080", "Server URL");
    model_cmd.add_subcommand("pull", "Pull a model from a registry")
        .add_option("--server", &server_url, "http://localhost:8080", "Server URL")
        .add_argument("name", &model_name, "Model name");
    model_cmd.add_subcommand("remove", "Delete a model")
        .add_option("--server", &server_url, "http://localhost:8080", "Server URL")
        .add_argument("name", &model_name, "Model name");
    model_cmd.add_subcommand("show", "Show model details")
        .add_option("--server", &server_url, "http://localhost:8080", "Server URL")
        .add_argument("name", &model_name, "Model name");

    auto* cmd = app.parse(argc, argv);

    if (cmd->name == "_global" || cmd->name == "") {
        app.print_help();
        return 0;
    }

    auto full = cmd->full_name();

    if (full == "serve list")
        return cmd_serve_list();
    if (full == "serve stop")
        return cmd_serve_stop(serve_stop_target);
    if (full == "serve") {
        serve_cmd.print_help("cllama");
        return 0;
    }
    if (full == "serve start")
        return cmd_serve(serve_name, host, port, models_folder, runner_binary_path(argv[0]), router_impl, daemon_flag, pidfile, log_file);

    if (full == "runner start")
        return cmd_runner_start(server_url, runner_name, runner_bin, runner_type, model_name);
    if (full == "runner list")
        return cmd_runner_list(server_url);
    if (full == "runner stop")
        return cmd_runner_stop(server_url, runner_name);
    if (full == "runner")
        return runner_cmd.print_help("cllama"), 0;
    if (full == "runner chat")
        return cmd_runner_chat(server_url, runner_name,
                               chat_max_tokens, chat_temp, chat_top_p, chat_top_k);

    if (full == "model list")
        return cmd_model_list(server_url);

    if (full == "model pull")
        return cmd_model_pull(server_url, model_name);

    if (full == "model remove")
        return cmd_model_remove(server_url, model_name);

    if (full == "model show")
        return cmd_model_show(server_url, model_name);

    app.print_help();
    return 0;
}
