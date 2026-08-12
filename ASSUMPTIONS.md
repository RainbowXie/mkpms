# ASSUMPTIONS

本仓库 Nightmanager/开发循环中记录的假设（无法验证时做出的合理默认，供后续修正）。

## 环境与验证

- **无真机验证环境**：`aarch64-linux-gnu-gcc` 未安装、`.kp` submodule 未初始化，无法交叉编译/真机测试内核模块。验证方式 = **host gcc -fsyntax-only + 真实 KernelPatch 框架头**（`scripts/kernel_syntax_check.sh`，**14 个 KPM 源文件全部通过**）+ host 侧单元测试 + 执行级 harness + 代码审查。
- **执行级 harness 抓出 3 个语法检查发现不了的真 bug**（Iteration 28-30）：`ghostmem_make_pte` 漏 `PTE_USER`（用户进程无法访问）、`do_alloc` prot==0 默认 RWX 语义缺失、harness 页对齐桩需模拟 `__get_free_pages` 语义。均已在主干修复。
- **验证体系单命令化**（Iteration 34）：根构建 `ctest` 统一跑 6 套件（81 断言）；KPM 模块 target 需交叉编译器（预期失败，host 测试 target 不受影响）。
- KernelPatch 源码本地副本位于 `/mnt/data/Work/Projects/KernelPatch`（脚本需指定其路径或初始化 submodule）。
- **宿主测试平台 = x86_64**：ghostlinker 的目标平台是 ARM64（Android），但 host gcc 产出 x86_64 ELF。为能在宿主机验证加载/重定位逻辑，ghostlinker 额外支持 `R_X86_64_*` 重定位类型（仅测试用途，非生产目标）。
- **x86_64 RELA 格式**：假定 `gcc -shared` 产出纯 RELA（addend 在 `r_addend`，slot 初始为 0）。已用 readelf/实测确认。

## ghostlinker v1 语义

- **外部符号缺失不致命**：未解析的外部符号 slot 置 0 并继续加载（打印警告）。只有内部符号解析失败才是硬错误。对应"v1 支持无外部依赖 payload"的范围。
- **不调用含外部依赖的导出函数**：PLT stub 未填充是 v1 非目标，测试只验证数据重定位与符号寻址，不跨外部依赖调用。
- **符号表条目数** = `(DT_STRTAB - DT_SYMTAB) / sizeof(Elf64_Sym)`，避免越界读 strtab（DT_HASH/DT_GNU_HASH 未实现）。

## ghostmem 内核模块

- **VMA 空洞扫描基址** = 4GB（`GHOSTMEM_SCAN_BASE`），上限 448GB：假设 Android 应用 mmap 区（~32GB+ 起）与堆（<4GB）之间的巨大空洞长期空闲。
- **fork 不继承幽灵映射**（无 VMA，子进程全新 mm）：符合预期，未做 dup_mmap 钩子。
- **无 mmap_read_lock**：`find_vma` 调用不持锁（wxshadow 同款 lockless 设计），存在与并发 mmap 的竞态窗口，已记录在 docs/ghostmem.md 限制节。

## 并发

- 仓库内存在用户自己的 codex 后台进程，可能并发提交。策略：不做 rebase/force-push，保留其提交（如 `ee0734e`）于历史中，工作树以我方版本为准。
