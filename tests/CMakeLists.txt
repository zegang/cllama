cmake_minimum_required(VERSION 3.15)

project(CLLaMA_TESTS VERSION 1.0.0 LANGUAGES CXX)

# Set C++ standard
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Enable testing
enable_testing()

# Source files
add_executable(test_api tests/test_api.cpp)

# Link with cllama library
target_link_libraries(test_api cllama_lib)

# Add test
add_test(NAME test_api COMMAND test_api)

# Install executables
install(TARGETS test_api DESTINATION bin)
