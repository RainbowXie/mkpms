add_kpm_module(ghostmem
    ghostmem.c
    ghostmem_pgtable.c
    ghostmem_scan.c
)

# Build client tool (userspace)
add_executable(ghostmem_client ghostmem_client.c)
target_link_options(ghostmem_client PRIVATE -static -s)
