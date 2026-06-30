#ifndef CLlama_API_CONFIG_HPP
#define CLlama_API_CONFIG_HPP

#include <string>
#include <map>
#include <vector>
#include <cstdlib>

namespace cllama {
namespace api {

struct ServerConfig {
    std::string host = "0.0.0.0";
    int port = 9027;
    std::string model_path = "/tmp/cllama/models";
    int max_request_workers = 4;
    int request_queue_max_size = 1000;
    int read_timeout_seconds = 30;
    int write_timeout_seconds = 30;
    int keep_alive_timeout_seconds = 75;
    std::vector<std::string> allowed_origins;
    std::string authentication_token;

    /// Override config fields from CLLAMA_* environment variables.
    static ServerConfig apply_env_overrides(ServerConfig cfg);
};

} // namespace api
} // namespace cllama

#endif // CLlama_API_CONFIG_HPP
