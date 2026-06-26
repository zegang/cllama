cmake_minimum_required(VERSION 3.15)
project(CLLaMA VERSION 1.0.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(CMAKE_BUILD_TYPE STREQUAL "")
    set(CMAKE_BUILD_TYPE Release)
endif()

# ── Platform-wide definitions ──────────────────────────────────
if(WIN32)
    add_definitions(-DWIN32_LEAN_AND_MEAN -DNOMINMAX)
endif()

find_package(Threads REQUIRED)

# OpenSSL is optional — only needed by cpprestsdk for HTTPS support.
# cpprestsdk handles finding it internally when required (iOS, Android
# use prebuilt paths; Windows uses WinHTTP by default).
find_package(OpenSSL QUIET)

# ── Header-only dependencies (via FetchContent) ────────────
include(FetchContent)

# cpp-httplib – single-header HTTP client
FetchContent_Declare(httplib
    GIT_REPOSITORY https://github.com/yhirose/cpp-httplib.git
    GIT_TAG v0.18.2)
FetchContent_MakeAvailable(httplib)

# nlohmann/json – JSON library
FetchContent_Declare(nlohmann_json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG v3.11.3)
FetchContent_MakeAvailable(nlohmann_json)

# spdlog – fast, header-only logging library
FetchContent_Declare(spdlog
    GIT_REPOSITORY https://github.com/gabime/spdlog.git
    GIT_TAG v1.15.1)
FetchContent_MakeAvailable(spdlog)

# ── Submodules ──────────────────────────────────────────────
# llama.cpp – core LLM inference engine
add_subdirectory(third_party/llama.cpp)

# cpprestsdk – HTTP client/server framework
add_subdirectory(third_party/cpprestsdk)

# Suppress over-zealous GCC warnings in cpprestsdk
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
  target_compile_options(cpprest PRIVATE -Wno-error=format-truncation)
endif()

# oatpp – C++ web framework
add_subdirectory(third_party/oatpp)

# Suppress warnings in oatpp
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
  target_compile_options(oatpp PRIVATE -Wno-error=format-truncation -Wno-error=unused-variable)
endif()

# ── Application / Library ──────────────────────────────────
add_subdirectory(src)

# ── CLI ────────────────────────────────────────────────────
add_subdirectory(src/cli)

# ── Runner (child process for model inference) ──────────────
add_subdirectory(src/runner)
