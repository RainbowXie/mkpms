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
