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
