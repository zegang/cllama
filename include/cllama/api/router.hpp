#ifndef CLlama_API_ROUTER_HPP
#define CLlama_API_ROUTER_HPP

#include "cllama/api/config.hpp"
#include "cllama/core/runner_mgr.hpp"
#include <memory>

namespace cllama {
namespace api {

class Router {
public:
    Router(const ServerConfig& config, std::shared_ptr<RunnerManager> runner_mgr)
        : config_(config), runner_mgr_(std::move(runner_mgr)) {}
    virtual ~Router() = default;

    virtual void start() = 0;
    virtual void stop()  = 0;

protected:
    ServerConfig              config_;
    std::shared_ptr<RunnerManager> runner_mgr_;
};

} // namespace api
} // namespace cllama

#endif // CLlama_API_ROUTER_HPP
