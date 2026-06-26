#include <iostream>
#include <cassert>
#include <cllama/cllama.hpp>

using namespace cllama;

void test_backend_initialization() {
    std::cout << "Testing backend initialization..." << std::endl;
    
    auto backend = std::make_unique<OllamaBackend>("http://localhost:11434");
    assert(backend->name() == "ollama_backend");
    assert(backend->version() == "1.0.0");
    
    std::cout << "✓ Backend initialized successfully" << std::endl;
}

void test_model_info() {
    std::cout << "Testing model info..." << std::endl;
    
    ModelInfo info;
    info.name = "test-model";
    info.size = "1.0GB";
    info.family = "llama";
    info.quantization = "f16";
    
    Model model(info, true);
    assert(model.name() == "test-model");
    assert(model.loaded() == true);
    assert(model.info().size() == "1.0GB");
    
    std::cout << "✓ Model info test passed" << std::endl;
}

void test_completion_options() {
    std::cout << "Testing completion options..." << std::endl;
    
    CompletionOptions options;
    assert(options.max_tokens == 100);
    assert(options.temperature == 0.8f);
    assert(options.top_p == 0.95f);
    assert(options.top_k == 40);
    assert(options.seed == -1);
    assert(options.stream == false);
    
    options.max_tokens = 200;
    options.temperature = 1.0f;
    assert(options.max_tokens == 200);
    assert(options.temperature == 1.0f);
    
    std::cout << "✓ Completion options test passed" << std::endl;
}

void test_message() {
    std::cout << "Testing message structure..." << std::endl;
    
    Message msg;
    msg.role = "user";
    msg.content = "Hello, world!";
    
    assert(msg.role == "user");
    assert(msg.content == "Hello, world!");
    
    std::cout << "✓ Message test passed" << std::endl;
}

int main() {
    std::cout << "=== CLLaMA Tests ===" << std::endl;
    
    try {
        test_backend_initialization();
        test_model_info();
        test_completion_options();
        test_message();
        
        std::cout << "\n✓ All tests passed!" << std::endl;
        return 0;
    } catch (const std::exception& e) {
        std::cout << "\n✗ Test failed: " << e.what() << std::endl;
        return 1;
    }
}
