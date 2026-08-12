# Spec: KPM 幽灵内存子系统 ghostmem

Status: active
Owner: ethan
Created: 2025-08-12

## Problem

视频方案（`../solution/`）核心之一：内核 KPM 直接分配物理页、手动建立 PTE 页表项，得到不登记在 `/proc/self/maps` 的 **VMA-Less 幽灵内存**，作为后续无痕跳板（stealth trampolines）与自定义 Linker 的内存底座。当前 mkpms 仅有 wxshadow（W^X Shadow 断点），缺少这块"内核侧无痕内存分配"能力。

## Goals

- 新增 KPM 模块 `ghostmem`：为指定进程分配/释放 VMA-Less RWX 幽灵内存（物理页 + 手动 PTE，不创建 VMA）。
- 提供 `prctl` 用户接口：alloc / free / info，供用户态工具与 Frida Gum 对接。
- 生命周期安全：进程退出（`exit_mmap`）自动释放全部幽灵页，卸载模块清理全部遗留。
- 提供静态用户态客户端 `ghostmem_client`：封装 prctl 接口，可分配、读写、释放、验证 maps 不可见。
- 提供自定义 Linker 工具 `ghostlinker`（v1）：在幽灵内存中手工映射最小 ELF —— PT_LOAD 段映射、RELA 重定位、符号解析回调、init_array 调用。
- 文档：`docs/PLAN.md`（已有）、`docs/ghostmem.md`、`docs/ghostlinker.md`。

## Non-Goals

- 不实现 TLS（Thread Local Storage）初始化 —— v1 明确不支持，文档标注。
- 不做 Gum 侧 `gum_set_stealth_alloc` 对接（属 rustFrida Phase 2）。
- 不隐藏 `pagemap` PFN 可见性（`follow_page_pte` 级隐藏是后续工作）。
- 不修改 `kernel/` 框架代码与既有 wxshadow 模块行为。

## Current Behavior

- `kpms/wxshadow/` 已有完整的手动 PTE 基础设施可复用模式：`get_user_pte`、`make_pte`、`wxshadow_flush_tlb_page`、`is_kva`、`vaddr_to_paddr_at`、`__get_free_pages`/`free_pages` 符号解析、`hook_syscalln(__NR_prctl,...)`、`exit_mmap_before` 生命周期钩子。
- 无任何 VMA-Less 分配能力。

## Desired Behavior

**prctl 接口**（新常量族 `0x47474d01..`，`GM` = GhostMem 前缀）：
- `PR_GHOSTMEM_ALLOC (0x47474d01)`：`prctl(opt, pid, nr_pages, prot, 0)`，返回用户态 VA（long）；`prot` 支持 `PROT_READ/WRITE/EXEC` 组合，默认 RWX。
- `PR_GHOSTMEM_FREE (0x47474d02)`：`prctl(opt, pid, va, 0, 0)`，释放单页或整块。
- `PR_GHOSTMEM_INFO (0x47474d03)`：`prctl(opt, pid, buf, len, 0)`，向用户缓冲区写统计（总页数/块数/占用）。
- 分配在目标进程 VMA 空洞中选地址（不与其他 VMA 重叠、避开 `mmap_min_addr`），手动写 PTE 并 TLB flush。
- 每块记录 `(mm, va, nr_pages, pfn)` 于全局链表，`exit_mmap` 钩子 + 模块退出时清理。

**ghostmem_client**（`-static -s`，同 wxshadow_client 风格）：
- `-p <pid> -a <nr_pages> [--prot rwx]` 分配；`-f <va>` 释放；`-d <va> <hex>` 写；`-r <va> <len>` 读（hex dump）；`-i` 信息；`-c <va>` 校验 maps 中无此区间。

**ghostlinker v1**（静态工具 + 库式 API）：
- 输入：ELF64（ET_DYN）路径或内存 buffer；输出：映射到幽灵内存的基址 + 导出符号表。
- 步骤：解析 ELF header/program headers → 按 `PT_LOAD` 段对齐映射（文件偏移/内存偏移/权限）→ 处理 `PT_DYNAMIC`（`DT_RELA`/`DT_RELACOUNT`、`DT_INIT_ARRAY` 等）→ RELA 重定位（`R_AARCH64_RELATIVE`、`R_AARCH64_GLOB_DAT`、`R_AARCH64_JUMP_SLOT` + 解析器回调）→ 调用 `init_array`。
- 符号解析：无外部依赖的 stub 直接完成；外部符号通过回调由宿主提供（v1 语义）。
- 明确文档化非目标：TLS、IFUNC、异常展开表链接。

## Acceptance Criteria

- [ ] `ghostmem` KPM 模块可编译为目标文件（语法/类型正确，遵循 wxshadow 符号解析与 hook 模式）；`prctl` 三个接口常量与文档一致。
- [ ] 分配路径：`__get_free_pages` + 手动 PTE（`PTE_USER|PTE_AF|PTE_NG|ATTRINDX_NORMAL`，W=1 可写、UXN=0 可执行）+ TLB flush；地址从 VMA 空洞选取；块登记链表。
- [ ] 释放路径：恢复/清除 PTE、TLB flush、`free_pages`，链表摘除；幂等（重复释放返回错误而非崩溃）。
- [ ] 生命周期：`exit_mmap` 钩子释放该 mm 全部幽灵块；模块退出清理全部遗留且先 unhook。
- [ ] `ghostmem_client` 编译通过（host gcc 语法检查），参数解析与 wxshadow_client 风格一致。
- [ ] `ghostlinker` 编译通过；对最小 ET_DYN stub 完成段映射 + RELATIVE 重定位 + init_array 调用的代码路径完整。
- [ ] 文档 `docs/ghostmem.md`/`docs/ghostlinker.md` 覆盖原理、API、使用示例、限制。

## Edge Cases

- 目标 pid 非法 / 已退出 / 无 mm：返回 `-ESRCH`。
- 分配时无足够 VMA 空洞：返回 `-ENOMEM`。
- 跨页连续分配：块内页数 > 1 时 PTE 连续铺设；VA 对齐页。
- free 传入未登记 VA：返回 `-EINVAL`，不崩溃。
- fork 后子进程：不继承幽灵映射（子进程 mm 全新，PTE 不复制 —— 文档说明）。
- 并发 prctl：全局链表用锁；页表操作无锁但每页 PTE 重写用原子自旋（wxshadow 模式）。

## Suggested Approach

- 目录 `kpms/ghostmem/`：`ghostmem.h`（公共接口/常量）、`ghostmem.c`（核心：链表/分配/释放/prctl/hook）、`ghostmem_pgtable.c`（PTE 操作，尽量复用/参照 `wxshadow_pgtable.c` 模式）、`ghostmem_client.c`（用户态工具）。
- CMake：`add_kpm_module(ghostmem ghostmem.c ghostmem_pgtable.c)` + `add_executable(ghostmem_client ...)` 静态链接。
- 符号解析参照 wxshadow：`extern void *(*kfunc_*)(...)` + `resolve_symbols()`。
- 工具 `tools/ghostlinker/`：`ghostlinker.c` + `CMakeLists.txt`（host 可编译的静态工具，含库式 API `gh_link_elf()`）。

## Testing Plan

No automated validation commands configured for this repository.

Manual checks (spec-specific):
- `gcc -fsyntax-only kpms/ghostmem/ghostmem_client.c` —— 客户端语法检查（host gcc 可用）。
- `gcc -fsyntax-only tools/ghostlinker/ghostlinker.c`（如无内核依赖）—— linker 语法检查。
- 内核模块源文件人工审查：类型/常量/符号解析对照 wxshadow 模式。
- 真机验证（需 aarch64-linux-gnu-gcc + APatch 环境，本环境不可用）：`kpatch module load ghostmem.kpm` 后运行 `ghostmem_client -p <pid> -a 16`，`cat /proc/<pid>/maps | grep <va>` 应无输出。

## Documentation Updates

- `docs/PLAN.md`（已有，含路线图）
- `docs/ghostmem.md`（新）
- `docs/ghostlinker.md`（新）
- `README.md`：模块列表与快速使用一节。

## Risks / Open Questions

- 无真机验证环境：内核路径以代码审查 + 语法检查兜底，风险记录在案。
- VMA-less 映射在部分内核路径（如 GUP/进程切换时的 TLB）有未覆盖场景，按视频框架同类方案评估可接受。
- ghostlinker v1 仅支持无 TLS 依赖的 payload，符号解析依赖宿主回调。
