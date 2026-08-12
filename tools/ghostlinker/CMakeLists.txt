# GhostLinker - host 侧工具（独立于 KPM 构建）
# 构建: cmake -S tools/ghostlinker -B build/ghostlinker && make
cmake_minimum_required(VERSION 3.10)
project(ghostlinker C)

add_library(ghostlinker STATIC ghostlinker.c)
target_include_directories(ghostlinker PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

add_executable(ghostlinker_cli ghostlinker_cli.c)
target_link_libraries(ghostlinker_cli ghostlinker)

# Host unit test: 编译 payload.so 后运行
add_executable(ghostlinker_test ghostlinker_test.c)
target_link_libraries(ghostlinker_test ghostlinker)

# 生成测试用 payload（纯数据 + 导出函数，无外部依赖）
set(GHL_PAYLOAD_SRC ${CMAKE_CURRENT_BINARY_DIR}/ghl_payload.c)
file(WRITE ${GHL_PAYLOAD_SRC}
"int g_value = 42;\n"
"int *g_ptr = &g_value;\n"
"int exported_add(int a, int b) { return a + b; }\n")
add_library(ghl_payload SHARED ${GHL_PAYLOAD_SRC})

add_test(NAME ghostlinker_test COMMAND ghostlinker_test $<TARGET_FILE:ghl_payload>)
enable_testing()
