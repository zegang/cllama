cmake_minimum_required(VERSION 3.20)
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
# Allow reusing an external oatpp build to avoid duplicate compilation.
# Set CLLAMA_OATPP_SOURCE_DIR to the oatpp source root (e.g. ../../vendor/oatpp).
if(CLLAMA_OATPP_SOURCE_DIR)
    message(STATUS "CLLaMA: Using external oatpp at ${CLLAMA_OATPP_SOURCE_DIR}")
    add_subdirectory(${CLLAMA_OATPP_SOURCE_DIR} ${CMAKE_BINARY_DIR}/oatpp)
else()
    add_subdirectory(third_party/oatpp)
endif()

# Suppress warnings in oatpp
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
  target_compile_options(oatpp PRIVATE -Wno-error=format-truncation -Wno-error=unused-variable)
endif()

# oatpp-swagger – OpenAPI/Swagger support
set(OATPP_MODULES_LOCATION "CUSTOM" CACHE STRING "Location where to find oatpp modules")
set(OATPP_DIR_SRC "${CMAKE_SOURCE_DIR}/third_party/oatpp/src" CACHE STRING "Path to oatpp source dir")
if(EXISTS "${CMAKE_SOURCE_DIR}/third_party/oatpp-swagger")
  add_subdirectory(third_party/oatpp-swagger)
else()
  FetchContent_Declare(oatpp-swagger
      GIT_REPOSITORY https://github.com/oatpp/oatpp-swagger.git
      GIT_TAG 1.3.1)
  FetchContent_MakeAvailable(oatpp-swagger)
endif()

# Pass swagger-ui resource path downstream
if(oatpp-swagger_SOURCE_DIR)
  set(OATPP_SWAGGER_RES_DIR "${oatpp-swagger_SOURCE_DIR}/res" CACHE INTERNAL "")
else()
  set(OATPP_SWAGGER_RES_DIR "${CMAKE_SOURCE_DIR}/third_party/oatpp-swagger/res" CACHE INTERNAL "")
endif()

# ── Application / Library ──────────────────────────────────
add_subdirectory(src)

# ── CLI ────────────────────────────────────────────────────
add_subdirectory(src/cli)

# ── Runner (child process for model inference) ──────────────
add_subdirectory(src/runner)
