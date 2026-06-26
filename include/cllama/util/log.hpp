#pragma once
#include <memory>
#include <string>

namespace spdlog {
class logger;
}

namespace cllama::util {

/// Initialize the logging system.
/// Reads CLLAMA_LOG_LEVEL (trace,debug,info,warn,err,critical,off) — default: info.
/// Reads CLLAMA_LOG_FILE — optional path to a rotating log file (5 MB × 3 files).
/// Must be called once before any logger access.
void init_logger();

/// Return a named logger (created on first access).
/// The name appears as [component] in log output.
std::shared_ptr<spdlog::logger> get_logger(const std::string& name);

/// Root/default logger.
std::shared_ptr<spdlog::logger> root_logger();

/// Add a rotating file sink (5 MB × 3 files) to all registered loggers.
/// Can be called after init_logger().
void set_log_file(const std::string& path);

} // namespace cllama::util
