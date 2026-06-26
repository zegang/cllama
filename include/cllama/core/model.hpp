#ifndef CLlama_MODEL_HPP
#define CLlama_MODEL_HPP

#include "cllama/core/types.hpp"
#include "cllama/core/error.hpp"
#include <string>
#include <memory>

namespace cllama {

class Model {
public:
    Model(const ModelInfo& info, bool loaded = false)
        : info_(info), loaded_(loaded) {}
    
    const std::string& name() const { return info_.name; }
    bool loaded() const { return loaded_; }
    const ModelInfo& info() const { return info_; }
    
    void set_loaded(bool loaded) { loaded_ = loaded; }
    
private:
    ModelInfo info_;
    bool loaded_;
};

} // namespace cllama

#endif // CLlama_MODEL_HPP
