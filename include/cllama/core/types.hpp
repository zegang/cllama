#ifndef CLlama_TYPES_HPP
#define CLlama_TYPES_HPP

#include <string>
#include <vector>
#include <optional>
#include <map>
#include <unordered_map>

namespace cllama {

struct Embedding {
    std::vector<float> data;
    int dimension;
};

struct Message {
    std::string role;
    std::string content;
};

struct CompletionOptions {
    int max_tokens = 100;
    float temperature = 0.8f;
    float top_p = 0.95f;
    int top_k = 40;
    float repeat_penalty = 1.1f;
    int seed = -1;
    bool stream = false;
    bool stop_allowed = true;
    int stop_token_count = 0;
    bool whisper = false;
    std::vector<std::string> stop_sequences;
};

struct ModelInfo {
    std::string name;
    std::string model_path;
    std::string family;
    std::string size;
    std::string file_format;
    std::string parameter_size;
    std::string quantization;
    std::string description;
    int32_t max_embedding_length;
};

using Token = int64_t;

} // namespace cllama

#endif // CLlama_TYPES_HPP
