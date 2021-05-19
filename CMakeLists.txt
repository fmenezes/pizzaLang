cmake_minimum_required(VERSION 2.8.12)
project(PizzaLang)

find_package(LLVM REQUIRED CONFIG)

message(STATUS "Found LLVM ${LLVM_PACKAGE_VERSION}")
message(STATUS "Using LLVMConfig.cmake in: ${LLVM_DIR}")

add_definitions(${LLVM_DEFINITIONS})
llvm_map_components_to_libnames(llvm_libs support core orcjit native)
include_directories(include ${LLVM_INCLUDE_DIRS} ${CMAKE_CURRENT_BINARY_DIR})

file(GLOB SOURCES "src/main.cpp" "src/pizza/*.cpp")

add_executable(bake ${SOURCES})

set_target_properties(bake PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
target_compile_features(bake PRIVATE cxx_std_14)

target_link_libraries(bake ${llvm_libs})

# On macOS, search Homebrew for keg-only versions of Bison and Flex. Xcode does
# not provide new enough versions for us to use.
if (CMAKE_HOST_SYSTEM_NAME MATCHES "Darwin")
    execute_process(
        COMMAND brew --prefix bison 
        RESULT_VARIABLE BREW_BISON
        OUTPUT_VARIABLE BREW_BISON_PREFIX
        OUTPUT_STRIP_TRAILING_WHITESPACE
    )
    if (BREW_BISON EQUAL 0 AND EXISTS "${BREW_BISON_PREFIX}")
        message(STATUS "Found Bison keg installed by Homebrew at ${BREW_BISON_PREFIX}")
        set(BISON_EXECUTABLE "${BREW_BISON_PREFIX}/bin/bison")
    endif()

    execute_process(
        COMMAND brew --prefix flex 
        RESULT_VARIABLE BREW_FLEX
        OUTPUT_VARIABLE BREW_FLEX_PREFIX
        OUTPUT_STRIP_TRAILING_WHITESPACE
    )
    if (BREW_FLEX EQUAL 0 AND EXISTS "${BREW_FLEX_PREFIX}")
        message(STATUS "Found Flex keg installed by Homebrew at ${BREW_FLEX_PREFIX}")
        set(FLEX_EXECUTABLE "${BREW_FLEX_PREFIX}/bin/flex")
        set(FLEX_INCLUDE_DIRS "${BREW_FLEX_PREFIX}/include")
        set(FL_LIBRARY "${BREW_FLEX_PREFIX}/lib/libfl.dylib")
    endif()
endif()

find_package(BISON REQUIRED)
find_package(FLEX REQUIRED)

BISON_TARGET(PARSER generators/parser.yy ${CMAKE_CURRENT_BINARY_DIR}/parser.cpp)

flex_target(LEXER generators/lexer.l ${CMAKE_CURRENT_BINARY_DIR}/lexer.cpp)

add_flex_bison_dependency(LEXER PARSER)


add_executable(bison-test src/parseapp.cpp src/driver.cpp ${BISON_PARSER_OUTPUTS} ${FLEX_LEXER_OUTPUTS})
target_include_directories(bison-test PUBLIC include ${CMAKE_CURRENT_BINARY_DIR})
target_link_libraries(bison-test ${FLEX_LIBRARIES})
set_target_properties(bison-test PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

target_compile_features(bison-test PRIVATE cxx_std_14)

target_link_libraries(bison-test ${llvm_libs})
