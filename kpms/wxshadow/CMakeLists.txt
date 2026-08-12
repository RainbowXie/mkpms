add_kpm_module(wxshadow
    wxshadow.c
    wxshadow_pgtable.c
    wxshadow_handlers.c
    wxshadow_bp.c
    wxshadow_scan.c
)

# Build client tool (userspace)
add_executable(wxshadow_client wxshadow_client.c)
target_link_options(wxshadow_client PRIVATE -static -s)

# wxshadow 跨仓库 prctl 常量对齐自检（mkpms <-> rustFrida）
enable_testing()
add_executable(wxshadow_abi_test tests/wxshadow_abi_test.c)
target_include_directories(wxshadow_abi_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/../ghostmem/tests/shim
)
add_test(NAME wxshadow_abi_test COMMAND wxshadow_abi_test)

# wxshadow_client 行为 e2e（host：-h/-t/失败路径）
add_executable(wxshadow_client_e2e_test tests/wxshadow_client_e2e_test.c)
add_test(NAME wxshadow_client_e2e_test
        COMMAND wxshadow_client_e2e_test $<TARGET_FILE:wxshadow_client>)
add_dependencies(wxshadow_client_e2e_test wxshadow_client)

# wxshadow PTE 状态机逻辑执行级测试（restore/hidden/stepping 位语义）
add_executable(wxshadow_pte_state_test tests/wxshadow_pte_state_test.c)
add_test(NAME wxshadow_pte_state_test COMMAND wxshadow_pte_state_test)

# wxshadow BRK 编码 + ESR 分类逻辑测试（断点识别核心）
add_executable(wxshadow_brk_esr_test tests/wxshadow_brk_esr_test.c)
add_test(NAME wxshadow_brk_esr_test COMMAND wxshadow_brk_esr_test)

# wxshadow TLB flush 三级回退优先级测试
add_executable(wxshadow_tlb_test tests/wxshadow_tlb_test.c)
add_test(NAME wxshadow_tlb_test COMMAND wxshadow_tlb_test)

# wxshadow task 链表遍历（字节偏移算术，历史 bug 回归）
add_executable(wxshadow_next_task_test tests/wxshadow_next_task_test.c)
add_test(NAME wxshadow_next_task_test COMMAND wxshadow_next_task_test)

# wxshadow 地址翻译助手（is_kva / v↔p 三模式）
add_executable(wxshadow_addr_test tests/wxshadow_addr_test.c)
add_test(NAME wxshadow_addr_test COMMAND wxshadow_addr_test)

# VMA 偏移助手（vm_start/vm_end 读取，ghostmem/wxshadow 共用）
add_executable(vma_offset_test tests/vma_offset_test.c)
add_test(NAME vma_offset_test COMMAND vma_offset_test)
