# Runner executable — spawned as a child process by the server
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
add_executable(cllama_runner llama_runner.cpp ../util/log.cpp)
target_include_directories(cllama_runner PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../../include)
target_link_libraries(cllama_runner PRIVATE
    llama
    httplib::httplib
    nlohmann_json::nlohmann_json
    spdlog::spdlog
    Threads::Threads)
install(TARGETS cllama_runner DESTINATION bin)