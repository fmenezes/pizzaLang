cmake_minimum_required(VERSION 2.8.12)
project(PizzaLang)

include_directories(include)

file(GLOB SOURCES "src/*.cpp")

add_executable(bake ${SOURCES})

set_target_properties(bake PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
target_compile_features(bake PRIVATE cxx_std_14)
