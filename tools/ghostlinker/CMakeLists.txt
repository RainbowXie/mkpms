# GhostLinker - host 侧工具（KPM 构建外或根构建内均可）
# 独立构建: cmake -S tools/ghostlinker -B build/ghostlinker && make
# 根构建内: 由根 CMakeLists add_subdirectory 引入（不重复 project）

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    cmake_minimum_required(VERSION 3.10)
    project(ghostlinker C)
endif()

add_library(ghostlinker STATIC ghostlinker.c)
target_include_directories(ghostlinker PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

add_executable(ghostlinker_cli ghostlinker_cli.c)
target_link_libraries(ghostlinker_cli ghostlinker)

# Host unit test: 编译 payload.so 后运行
add_executable(ghostlinker_test ghostlinker_test.c)
target_link_libraries(ghostlinker_test ghostlinker)

# 生成测试用 payload（纯数据 + 导出函数 + 无依赖构造器）
set(GHL_PAYLOAD_SRC ${CMAKE_CURRENT_BINARY_DIR}/ghl_payload.c)
file(WRITE ${GHL_PAYLOAD_SRC}
"int g_value = 42;\n"
"int *g_ptr = &g_value;\n"
"volatile int g_ctor_flag;\n"
"__attribute__((constructor)) void my_ctor(void) { g_ctor_flag = 0xCAFE; }\n"
"int exported_add(int a, int b) { return a + b; }\n")
add_library(ghl_payload SHARED ${GHL_PAYLOAD_SRC})

add_test(NAME ghostlinker_test COMMAND ghostlinker_test $<TARGET_FILE:ghl_payload>)
# 确保测试运行时 payload 已构建（ctest 不自动构建依赖）
add_dependencies(ghostlinker_test ghl_payload)

# CLI --run 测试：加载 payload 并调用导出构造器（验证输出含 calling init）
add_test(NAME ghostlinker_cli_run
        COMMAND sh -c "$<TARGET_FILE:ghostlinker_cli> $<TARGET_FILE:ghl_payload> --run | grep -q 'calling init:'")

# 非零 load_base 场景：-Ttext-segment 偏移首段 vaddr
set(GHL_OFFBASE_SRC ${CMAKE_CURRENT_BINARY_DIR}/ghl_offbase.c)
file(WRITE ${GHL_OFFBASE_SRC}
"int g_value = 42;\n"
"int *g_ptr = &g_value;\n"
"int exported_add(int a, int b) { return a + b; }\n")
add_library(ghl_offbase SHARED ${GHL_OFFBASE_SRC})
set_target_properties(ghl_offbase PROPERTIES
    LINK_FLAGS "-Wl,-Ttext-segment=0x400000")
add_executable(ghostlinker_offbase_test tests/ghostlinker_offbase_test.c)
target_link_libraries(ghostlinker_offbase_test ghostlinker)
add_test(NAME ghostlinker_offbase_test
        COMMAND ghostlinker_offbase_test $<TARGET_FILE:ghl_offbase>)
add_dependencies(ghostlinker_offbase_test ghl_offbase)

# BSS 清零路径（memsz > filesz 段）
set(GHL_BSS_SRC ${CMAKE_CURRENT_BINARY_DIR}/ghl_bss.c)
file(WRITE ${GHL_BSS_SRC}
"int g_value = 42;\n"
"int g_bss[256];\n"
"int exported_bss_sum(void) { int s=0; for(int i=0;i<256;i++) s+=g_bss[i]; return s; }\n")
add_library(ghl_bss SHARED ${GHL_BSS_SRC})
add_executable(ghostlinker_bss_test tests/ghostlinker_bss_test.c)
target_link_libraries(ghostlinker_bss_test ghostlinker)
add_test(NAME ghostlinker_bss_test
        COMMAND ghostlinker_bss_test $<TARGET_FILE:ghl_bss>)
add_dependencies(ghostlinker_bss_test ghl_bss)
