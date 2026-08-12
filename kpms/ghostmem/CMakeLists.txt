# VMA-Less Ghost Memory KPM module.
# The user-space client (ghostmem_client) is out of scope for this commit.
add_kpm_module(ghostmem
    ghostmem.c
    ghostmem_pgtable.c
)
