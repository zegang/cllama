cmake_minimum_required(VERSION 3.15)

project(CLLaMA_SRC VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)

add_subdirectory(platform)

add_library(cllama_lib
    api/router.cpp
    api/cpprest_router.cpp
    api/oatpp_router.cpp
    api/config.cpp
    core/model.cpp
    core/runner_mgr.cpp
    util/log.cpp
    ${CLLAMA_PLATFORM_SRC}
)

target_include_directories(cllama_lib PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/../include)

target_link_libraries(cllama_lib PUBLIC
    Threads::Threads
    llama
    cpprest
    oatpp
    httplib::httplib
    nlohmann_json::nlohmann_json
    spdlog::spdlog
)

if(WIN32 AND BUILD_SHARED_LIBS)
    target_compile_definitions(cllama_lib PRIVATE CLLAMA_EXPORTS)
endif()

set_target_properties(cllama_lib PROPERTIES
    POSITION_INDEPENDENT_CODE ON
    OUTPUT_NAME cllama
)

install(TARGETS cllama_lib
    EXPORT CLLaMAPublicTargets
    LIBRARY DESTINATION lib
    ARCHIVE DESTINATION lib
    RUNTIME DESTINATION bin
)

install(DIRECTORY ../../include/cllama/ DESTINATION include/cllama
    FILES_MATCHING PATTERN "*.hpp"
)
