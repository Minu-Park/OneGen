cmake_minimum_required(VERSION 3.24)
project(OneGen VERSION 0.3.2 LANGUAGES CXX)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

enable_testing()
add_subdirectory(C++)
