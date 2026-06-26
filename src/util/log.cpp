#include <cllama/util/log.hpp>

#include <spdlog/sinks/rotating_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

#include <cstdlib>
#include <string>

namespace cllama::util {
namespace {

std::shared_ptr<spdlog::logger> g_root;
bool                            g_inited = false;

spdlog::level::level_enum parse_level(const char* env) {
    if (!env) return spdlog::level::info;
    std::string s(env);
    if (s == "trace")    return spdlog::level::trace;
    if (s == "debug")    return spdlog::level::debug;
    if (s == "info")     return spdlog::level::info;
    if (s == "warn")     return spdlog::level::warn;
    if (s == "err")      return spdlog::level::err;
    if (s == "critical") return spdlog::level::critical;
    if (s == "off")      return spdlog::level::off;
    return spdlog::level::info;
}

} // anonymous namespace

void init_logger() {
    if (g_inited) return;
    g_inited = true;

    auto level = parse_level(std::getenv("CLLAMA_LOG_LEVEL"));

    // stderr colour sink — never pollutes stdout
    auto console = std::make_shared<spdlog::sinks::stderr_color_sink_mt>();
    console->set_level(level);

    std::vector<spdlog::sink_ptr> sinks{console};

    // optional rotating file sink
    auto log_file = std::getenv("CLLAMA_LOG_FILE");
    if (log_file && log_file[0]) {
        auto file = std::make_shared<spdlog::sinks::rotating_file_sink_mt>(
            log_file, 5 * 1024 * 1024, 3);
        file->set_level(level);
        sinks.push_back(file);
    }

    g_root = std::make_shared<spdlog::logger>("root", sinks.begin(), sinks.end());
    g_root->set_level(level);
    g_root->flush_on(level);
    g_root->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%n] [%^%l%$] %v");

    spdlog::register_logger(g_root);
}

std::shared_ptr<spdlog::logger> get_logger(const std::string& name) {
    auto l = spdlog::get(name);
    if (l) return l;
    // create a child logger that shares sinks with root
    l = std::make_shared<spdlog::logger>(name, g_root->sinks().begin(),
                                         g_root->sinks().end());
    l->set_level(g_root->level());
    l->flush_on(g_root->level());
    l->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%n] [%^%l%$] %v");
    spdlog::register_logger(l);
    return l;
}

std::shared_ptr<spdlog::logger> root_logger() {
    return g_root ? g_root : spdlog::default_logger();
}

void set_log_file(const std::string& path) {
    if (!g_root || path.empty()) return;
    auto file = std::make_shared<spdlog::sinks::rotating_file_sink_mt>(
        path, 5 * 1024 * 1024, 3);
    file->set_level(g_root->level());
    // child loggers created by get_logger() each hold their own copy of the
    // sink vector, so we must push the file sink onto EVERY registered logger
    spdlog::apply_all([&](std::shared_ptr<spdlog::logger> l) {
        l->sinks().push_back(file);
    });
}

} // namespace cllama::util
