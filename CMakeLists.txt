cmake_minimum_required(VERSION 3.24)

project(matchbook VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(MATCHBOOK_BUILD_TESTS "Build unit tests" ON)
option(MATCHBOOK_NATIVE "Compile with -march=native" OFF)
option(MATCHBOOK_SANITIZE "Build with AddressSanitizer + UBSan" OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

# Warnings stay on for everything we own; third-party targets get their own flags.
add_library(matchbook_warnings INTERFACE)
if(MSVC)
  target_compile_options(matchbook_warnings INTERFACE /W4 /permissive-)
else()
  target_compile_options(matchbook_warnings INTERFACE -Wall -Wextra -Wpedantic -Wshadow -Wconversion)
endif()

if(MATCHBOOK_SANITIZE)
  if(MSVC OR MINGW)
    message(FATAL_ERROR "MATCHBOOK_SANITIZE needs GCC/Clang on Linux or macOS")
  endif()
  # applied globally so gtest and our code agree on the runtime
  add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all)
  add_link_options(-fsanitize=address,undefined)
endif()

configure_file(include/matchbook/version.hpp.in
               ${CMAKE_CURRENT_BINARY_DIR}/generated/matchbook/version.hpp @ONLY)

add_library(matchbook
  src/price.cpp
)
target_include_directories(matchbook
  PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_BINARY_DIR}/generated
)
target_link_libraries(matchbook PRIVATE matchbook_warnings)
if(MATCHBOOK_NATIVE AND NOT MSVC)
  target_compile_options(matchbook PRIVATE -march=native)
endif()

add_executable(matchbook_cli tools/matchbook_cli.cpp)
set_target_properties(matchbook_cli PROPERTIES OUTPUT_NAME matchbook)
target_link_libraries(matchbook_cli PRIVATE matchbook matchbook_warnings)

if(MATCHBOOK_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
