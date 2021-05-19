cmake_minimum_required(VERSION 2.8.12)
project(PizzaLang)

find_package(LLVM REQUIRED CONFIG)

message(STATUS "Found LLVM ${LLVM_PACKAGE_VERSION}")
message(STATUS "Using LLVMConfig.cmake in: ${LLVM_DIR}")

include_directories(include ${LLVM_INCLUDE_DIRS})
add_definitions(${LLVM_DEFINITIONS})

file(GLOB SOURCES "src/*.cpp" "src/**/*.cpp")

add_executable(bake ${SOURCES})

set_target_properties(bake PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
target_compile_features(bake PRIVATE cxx_std_14)

llvm_map_components_to_libnames(llvm_libs support core orcjit native)

target_link_libraries(bake ${llvm_libs})
