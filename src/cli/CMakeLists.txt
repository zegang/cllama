# CLI executables
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
add_executable(cllama main.cpp)
target_link_libraries(cllama PRIVATE cllama_lib)
install(TARGETS cllama DESTINATION bin)

