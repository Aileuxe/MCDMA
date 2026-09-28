cmake_minimum_required(VERSION 3.18)
project(mcdma_native_kv LANGUAGES CXX)

if(NOT LLAMA_SOURCE_DIR OR NOT LLAMA_BUILD_DIR)
    message(FATAL_ERROR "Set LLAMA_SOURCE_DIR and LLAMA_BUILD_DIR to the patched, pinned llama.cpp checkout and build")
endif()

add_executable(mcdma-native-kv native_kv.cpp)
add_executable(mcdma-native-tensor-probe native_tensor_probe.cpp)
add_executable(mcdma-native-latency native_latency.cpp)
foreach(target mcdma-native-kv mcdma-native-tensor-probe mcdma-native-latency)
target_compile_features(${target} PRIVATE cxx_std_17)
target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
target_include_directories(${target} PRIVATE
    "${LLAMA_SOURCE_DIR}/include" "${LLAMA_SOURCE_DIR}/src"
    "${LLAMA_SOURCE_DIR}/ggml/include" "${LLAMA_SOURCE_DIR}/ggml/src"
    "${LLAMA_SOURCE_DIR}/ggml/src/ggml-rpc")
foreach(lib llama ggml ggml-base ggml-rpc)
    string(REPLACE "-" "_" lib_var "${lib}")
    find_library(${lib_var}_PATH NAMES "${lib}" PATHS "${LLAMA_BUILD_DIR}/bin" NO_DEFAULT_PATH REQUIRED)
    target_link_libraries(${target} PRIVATE "${${lib_var}_PATH}")
endforeach()
set_target_properties(${target} PROPERTIES BUILD_RPATH "${LLAMA_BUILD_DIR}/bin")
endforeach()
