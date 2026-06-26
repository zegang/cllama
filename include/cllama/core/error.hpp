#ifndef CLlama_ERROR_HPP
#define CLlama_ERROR_HPP

#include <string>
#include <any>
#include <stdexcept>

namespace cllama {

class CLlamaError : public std::runtime_error {
public:
    explicit CLlamaError(const std::string& msg) : std::runtime_error(msg) {}
};

enum class ErrorCode {
    SUCCESS = 0,
    UNKNOWN_ERROR = 1,
    INVALID_ARGUMENT = 2,
    NOT_FOUND = 3,
    ALREADY_EXISTS = 4,
    PERMISSION_DENIED = 5,
    UNAVAILABLE = 6,
    MODEL_ERROR = 7,
    NETWORK_ERROR = 8,
};

struct Error {
    ErrorCode code;
    std::string message;

    Error(ErrorCode code, const std::string& message) : code(code), message(message) {}
};

template<typename T>
class CLlamaResult {
public:
    CLlamaResult(const T& value) : value_(value), error_(Error(ErrorCode::SUCCESS, "")) {}

    CLlamaResult(const Error& error) : error_(error) {}

    bool success() const { return error_.code == ErrorCode::SUCCESS; }

    const Error& error() const { return error_; }

    const T& get() const { return value_; }

private:
    T value_;
    Error error_;
};

} // namespace cllama

#endif // CLlama_ERROR_HPP
