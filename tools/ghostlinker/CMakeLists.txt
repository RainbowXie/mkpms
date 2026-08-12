# GhostLinker - host 侧工具（独立于 KPM 构建）
# 构建: cmake -S tools/ghostlinker -B build/ghostlinker && make
cmake_minimum_required(VERSION 3.10)
project(ghostlinker C)

add_library(ghostlinker STATIC ghostlinker.c)
target_include_directories(ghostlinker PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

add_executable(ghostlinker_cli ghostlinker_cli.c)
target_link_libraries(ghostlinker_cli ghostlinker)
