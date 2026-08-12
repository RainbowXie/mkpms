add_kpm_module(ghostmem
    ghostmem.c
    ghostmem_pgtable.c
    ghostmem_scan.c
)

# Build client tool (userspace)
add_executable(ghostmem_client ghostmem_client.c)
target_link_options(ghostmem_client PRIVATE -static -s)

# Host ABI self-check (内核头 + ktypes 桩，防止常量/布局漂移)
enable_testing()
add_executable(ghostmem_abi_test tests/ghostmem_abi_test.c)
target_include_directories(ghostmem_abi_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/shim
    ${CMAKE_CURRENT_SOURCE_DIR}
)
add_test(NAME ghostmem_abi_test COMMAND ghostmem_abi_test)

# ghostmem_client 区间重叠逻辑 host 单测
add_executable(ghostmem_client_overlap_test tests/ghostmem_client_overlap_test.c)
target_include_directories(ghostmem_client_overlap_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME ghostmem_client_overlap_test COMMAND ghostmem_client_overlap_test)

# ghostmem init args hex 解析 host 单测
add_executable(ghostmem_args_test tests/ghostmem_args_test.c)
target_include_directories(ghostmem_args_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME ghostmem_args_test COMMAND ghostmem_args_test)

# ghostmem_pgtable 逻辑执行级 harness（19 断言：map/unmap/reclaim/权限/共享表）
add_executable(ghostmem_pgtable_harness tests/ghostmem_pgtable_harness.c)
target_include_directories(ghostmem_pgtable_harness PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/shim
)
target_compile_definitions(ghostmem_pgtable_harness PRIVATE GHOSTMEM_PGTABLE_HARNESS)
add_test(NAME ghostmem_pgtable_harness COMMAND ghostmem_pgtable_harness)

# ghostmem.c 核心逻辑执行级 harness（15 断言：空洞查找/alloc/free/归一化）
add_executable(ghostmem_core_harness tests/ghostmem_core_harness.c)
target_include_directories(ghostmem_core_harness PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/shim
)
target_compile_definitions(ghostmem_core_harness PRIVATE GHOSTMEM_CORE_HARNESS)
add_test(NAME ghostmem_core_harness COMMAND ghostmem_core_harness)
