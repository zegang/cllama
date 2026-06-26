#ifndef CLlama_SESSION_HPP
#define CLlama_SESSION_HPP

#include "cllama/core/types.hpp"
#include "cllama/core/error.hpp"
#include <string>
#include <memory>
#include <chrono>
#include <ctime>

namespace cllama {

class Session {
public:
    Session(const std::string& session_id)
        : session_id_(session_id), created_at_(std::time(nullptr)) {}
    
    const std::string& id() const { return session_id_; }
    time_t created_at() const { return created_at_; }
    
    virtual ~Session() = default;
    
    virtual CLlamaResult<std::string> generate(
        const std::string& model,
        const std::string& prompt,
        const CompletionOptions& options) = 0;
    
    virtual CLlamaResult<std::string> chat(
        const std::string& model,
        const std::vector<Message>& messages,
        const CompletionOptions& options) = 0;
    
    virtual CLlamaResult<Embedding> embeddings(
        const std::string& model,
        const std::string& input) = 0;
    
    virtual CLlamaResult<bool> cancel() = 0;
    
    virtual bool is_active() const = 0;
    
protected:
    std::string session_id_;
    time_t created_at_;
};

using SessionPtr = std::unique_ptr<Session>;

} // namespace cllama

#endif // CLlama_SESSION_HPP
