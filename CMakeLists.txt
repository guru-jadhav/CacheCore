cmake_minimum_required(VERSION 3.20)

project(CacheCore)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

file(GLOB_RECURSE SOURCES src/*.cpp)

add_executable(CacheCore ${SOURCES})

target_include_directories(CacheCore PRIVATE ${PROJECT_SOURCE_DIR}/include)

target_link_libraries(CacheCore PRIVATE pthread)