# TODOs

Nightmanager implementation queue.

## Status Tags

- `[bug]` — eligible urgent defect; may omit spec and then uses `specs/TEMPLATE.md ## Testing Plan`.
- `[ready]` — eligible only with a non-draft linked spec.
- `[draft]` — not eligible until human-promoted.
- `[blocked]` — not eligible until reason resolved.
- `[in-progress]` — currently being worked.
- `[done]` — complete; include commit hash, and PR URL only if PR creation succeeds.

## Queue

- [done] ghostmem KPM 模块：VMA-Less 幽灵内存 + 手动 PTE + prctl 接口 (0b6386d)
  - Spec: `specs/ghostmem.md`
  - Scope: `kpms/ghostmem/` 新模块，内核态分配不登记 maps 的 RWX 内存；prctl 常量族 `0x47474d01..`；`exit_mmap` 钩子 + 模块退出清理。
  - Acceptance:
    - `add_kpm_module(ghostmem ...)` 接入 CMake；模块与 wxshadow 同模式（kfunc 符号解析、hook_syscalln prctl）。
    - alloc 在 VMA 空洞选地址，手动 PTE（USER|AF|NG|ATTRINDX_NORMAL，可写可执行）+ TLB flush。
    - free 幂等安全；exit_mmap 清理该 mm 全部块；模块退出先 unhook 再清理。
  - Notes: 无真机验证环境，按 spec Testing Plan 语法检查 + 代码审查兜底。
- [done] ghostmem_client 用户态工具 (0b6386d)
  - Spec: `specs/ghostmem.md`
  - Scope: `kpms/ghostmem/ghostmem_client.c`，`-static -s` 静态客户端（同 wxshadow_client 风格）。
  - Acceptance:
    - 参数：`-p pid -a nr_pages [--prot]`、`-f va`、`-w va hex`、`-r va len`、`-i`、`-c va`（校验 maps 不可见）。
    - host gcc 语法检查通过。
  - Notes: 客户端只依赖用户态 syscall prctl，可独立验证。
- [done] ghostlinker 自定义 Linker v1（幽灵内存 ELF 加载器）(b27eb80)
  - Spec: `specs/ghostmem.md`
  - Scope: `tools/ghostlinker/ghostlinker.c` + CMake；库式 API `gh_link_elf()`。
  - Acceptance:
    - ET_DYN 解析：PT_LOAD 段对齐映射、PT_DYNAMIC 扫描、RELA 重定位（RELATIVE/GLOB_DAT/JUMP_SLOT+回调）、init_array 调用。
    - host gcc 语法检查通过；`-h`/README 说明 TLS/IFUNC 为非目标。
  - Notes: v1 仅无 TLS 依赖 payload；符号解析走宿主回调。
- [done] ghostmem 技术文档 (2288311)
  - Spec: `specs/ghostmem.md`
  - Scope: `docs/ghostmem.md`（原理/prctl API/使用/限制）、`docs/ghostlinker.md`、`README.md` 模块列表。
  - Acceptance:
    - 覆盖原理（为何 maps 不可见）、prctl 三接口语义、client/linker 示例、真机验证步骤、已知限制。
  - Notes: 与 docs/PLAN.md 路线图一致。

<!-- Future phases (rustFrida, 需各自 Nightmanager 设置)：
- [draft] Stealth Trampolines：gum_set_stealth_alloc → PR_GHOSTMEM_ALLOC 对接
- [draft] Stalker + LZ4 实时压缩 trace 落盘对齐视频基准
-->
