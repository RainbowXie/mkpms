# 无痕 Hook 方案调研与实施计划

> 来源：`../solution/BV1eEuY6rE8s`（Bilibili 视频《安卓逆向打造零特征 Frida：内核 KPM + 幽灵内存 + 自定义 Linker》）
> 本仓库：`mkpms`（KernelPatch KPM 模块仓库）；兄弟仓库：`../rustFrida`（Frida Gum Rust 绑定 + 注入/trace）

## 1. 调研结论

视频展示的"零特征 Frida"框架，核心是把 Frida/Gum 的内存与执行特征从用户态彻底抹掉，对抗企业壳的源码级检测。共提炼 **9 个方案/能力点**：

| # | 方案 | 原理 | 检测对抗点 | 落点仓库 | 状态 |
|---|------|------|-----------|---------|------|
| 1 | **VMA-Less 幽灵内存** | 内核直接分配物理页、手动建 PTE，不创建 VMA → `/proc/self/maps` 不可见 | maps 扫描 SO/特征码 | mkpms | ✅ `kpms/ghostmem/`（7 prctl 接口：alloc/free/info/write/read + 跨仓库常量 4 副本锁定） |
| 2 | **手动 PTE 页表** | 绕过 `sys_mmap`，从 `mm->pgd` 手动遍历页表写 PTE | maps / 内核 VMA 审计 | mkpms | ✅ 含于 #1 |
| 3 | **自定义 Linker** | 在幽灵内存中手工映射 SO：解析 ELF、映射段、重定位 | soinfo / linker 符号比对 | mkpms | ✅ `tools/ghostlinker/`（v1，无 TLS） |
| 4 | **Stealth Trampolines** | 替换 Gum 跳板分配器（`gum_set_stealth_alloc`），跳板自动落幽灵内存 | 代码段特征扫描 | rustFrida | 🕐 分配器 ✅（`ghostmem.rs`）；`gum_set_stealth_alloc` 接线待 agent 构建链 |
| 5 | **W^X Shadow 断点** | shadow 页复制原始代码，断点写 shadow 页，读取切回原始页 | 指令内存比对（crc） | mkpms | ✅ 已有（wxshadow）；rustFrida `hook(ptr, cb, stealth)` → `wxshadow_patch` 已对接（回退 mprotect） |
| 6 | **ART 层特征隐蔽** | 隐藏 ART Hook 产生的运行时特征 | ART 内部检测 | rustFrida | ✅ 已有 `art_controller.rs`（ArtMethod 替换/OAT 隐藏/GC 同步）+ 设计文档（#4 结合点风险已分析） |
| 7 | **Native 无痕 Hook** | 跳板/补丁全部走幽灵内存，主/子线程稳定 | 崩溃/闪退 | 依赖 #1/#4 | 🕐 依赖 #4 完成 |
| 8 | **Stalker/QBDI 高速 Trace** | 指令级 trace + LZ4 实时压缩落盘 | 无（分析侧） | rustFrida | 🕐 Stalker LZ4 全链路 ✅（编码+落盘+host 解码器，e2e 验证）；QBDI 原生编码（基准角色）；基准对齐待真机 |
| 9 | **Zygote 注入与恢复** | 注入后恢复 Zygote 到干净状态 | zygote 状态检测 | rustFrida | ✅ 已有（loader.py） |

**视频实测数据（性能基准）**：Stalker 2936 万指令 7.45s（221.6 Minsn/s）、stop 落盘 118ms、QBDI 同任务 ~21.7s（慢 ~108 倍）。

## 2. 现状盘点

### mkpms（本仓库）
- `kpms/wxshadow/` —— **#5 W^X Shadow 已完整实现**（~7500 行）：shadow 页状态机、BRK/单步 handler、`do_page_fault`/`follow_page_pte` 隐藏、fork 保护、TLB 三级 fallback、prctl 接口 + 静态客户端。
- `kpms/ghostmem/` —— **#1/#2 幽灵内存已实现**：VMA-Less 分配/释放 + 手动 PTE（表页清零、dcache clean、空表回收）+ `prctl 0x47474d01..03` + `exit_mmap` 钩子 + 客户端 + ABI 自检（12 断言 ctest）。
- `tools/ghostlinker/` —— **#3 自定义 Linker v1 已实现**：幽灵内存中 ELF 映射 + RELA 重定位 + 符号回调 + init_array；12 项 host 单测（含与 lz4 无关的加载验证）。
- `kpms/hide-maps/`、`kpms/anti-detect/`、`kpms/demo-*` —— 辅助模块。

### rustFrida（兄弟仓库）
- `agent/src/`：`stalker.rs`（**LZ4 批落盘已接入**）、`trace/lz4_block.rs`（**新增**，7 测试含 lz4_flex 互操作）、`ghostmem.rs`（**新增**，#4 分配器）、`exec_mem.rs`、`quickjs_loader.rs`。
- `loader/`：`loader.py`/`loader.c` 注入器 —— #9 已有。
- `qbdi/`、`ldmonitor/` —— QBDI 集成与 linker 监控。
- **缺口**：#4 `gum_set_stealth_alloc` 注册（待 frida-gum 17.x + 构建链）、#7 依赖 #4、#8 真机基准对齐。

## 3. 实施路线图

### Phase 1（mkpms）：幽灵内存子系统 —— ✅ 完成
ghostmem KPM 模块（VMA-Less + 手动 PTE + prctl 接口 + 生命周期）、ghostmem_client、ghostlinker v1、文档、ABI 自检。PR #1。

### Phase 2（rustFrida）：Stealth Trampolines —— 🕐 进行中
`agent/src/ghostmem.rs` 分配器已实现（prctl 封装 + 3 测试）；`gum_set_stealth_alloc` 接线待 agent 构建链（需 NDK）。

### Phase 3（rustFrida）：Trace 工具链 —— 🕐 进行中
Stalker LZ4 全链路已完成：`lz4_block.rs`（7 测试含标准库互操作）+ 落盘接入（16B 块头）+ `trace-decoder` host 工具（端到端验证 3 块×8 指令精确还原）。QBDI 侧为原生事件编码（`qbdi-helper` writer，对比基准角色），不强加 LZ4。基准对齐视频数据需真机。

### Phase 4（rustFrida）：ART 特征隐蔽 —— 🕐 设计完成
ArtMethod entrypoint 版本无关扫描 + 幽灵内存跳板 + 原子还原（见 `docs/art-hiding.md`）；实现需 APatch 真机。

## 4. 通用约束

- 单文件 ≤ 500 行（强制 < 1000），超限即拆分。
- 内核模块遵循 wxshadow 既有模式：`wxfunc_*` 符号解析、`hook_wrap*`、`hook_syscalln`、无锁页表 + 细粒度锁。
- 注释遵循"写为什么"原则（code-comment-decision）。
- `kernel/` 框架目录只读参考，不得修改。

## 5. 风险与限制

- **无自动化验证**：环境缺 `aarch64-linux-gnu-gcc`、`.kp` submodule 未初始化，本批次按 spec Testing Plan 做静态/语法级人工检查。
- 幽灵内存是 VMA-less 映射：`access_ok`/`copy_to_user` 可用（页存在），但依赖该地址无 VMA 冲突 —— 分配需在 VMA 空洞中选取地址。
- `pagemap`/GUP 仍可见 PFN（视频框架同类局限）；`follow_page_pte` 级隐藏属于 Phase 2+。
- 自定义 Linker v1 仅覆盖无 TLS 依赖的 payload（如 stub/补丁代码）。
