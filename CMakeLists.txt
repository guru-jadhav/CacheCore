cmake_minimum_required(VERSION 3.20)

project(kv-store)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

file(GLOB_RECURSE SOURCES src/*.cpp)

add_executable(kv-store ${SOURCES})

target_include_directories(kv-store PRIVATE ${PROJECT_SOURCE_DIR}/include)

target_link_libraries(kv-store PRIVATE pthread)