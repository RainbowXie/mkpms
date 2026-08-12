# 无痕 Hook 方案调研与实施计划

> 来源：`../solution/BV1eEuY6rE8s`（Bilibili 视频《安卓逆向打造零特征 Frida：内核 KPM + 幽灵内存 + 自定义 Linker》）
> 本仓库：`mkpms`（KernelPatch KPM 模块仓库）；兄弟仓库：`../rustFrida`（Frida Gum Rust 绑定 + 注入/trace）

## 1. 调研结论

视频展示的"零特征 Frida"框架，核心是把 Frida/Gum 的内存与执行特征从用户态彻底抹掉，对抗企业壳的源码级检测。共提炼 **9 个方案/能力点**：

| # | 方案 | 原理 | 检测对抗点 | 落点仓库 |
|---|------|------|-----------|---------|
| 1 | **VMA-Less 幽灵内存** | 内核直接分配物理页、手动建 PTE，不创建 VMA → `/proc/self/maps` 不可见 | maps 扫描 SO/特征码 | mkpms（新） |
| 2 | **手动 PTE 页表** | 绕过 `sys_mmap`，从 `mm->pgd` 手动遍历页表写 PTE | maps / 内核 VMA 审计 | mkpms（新） |
| 3 | **自定义 Linker** | 在幽灵内存中手工映射 SO：解析 ELF、映射段、重定位、TLS 初始化 | soinfo / linker 符号比对 | mkpms（新） |
| 4 | **Stealth Trampolines** | 替换 Gum 跳板分配器（`gum_set_stealth_alloc`），`Interceptor.attach()`/Stalker 跳板自动落幽灵内存 | 代码段特征扫描 | rustFrida（对接） |
| 5 | **W^X Shadow 断点** | shadow 页复制原始代码，断点写 shadow 页（`--x`），读取时切回原始页（`r--`） | 指令内存比对（crc） | mkpms（已有：wxshadow） |
| 6 | **ART 层特征隐蔽** | 隐藏 ART Hook 产生的运行时特征 | ART 内部检测 | rustFrida（ART 模块） |
| 7 | **Native 无痕 Hook** | 跳板/补丁全部走幽灵内存，主/子线程稳定 | 崩溃/闪退 | 依赖 #1/#4 |
| 8 | **Stalker/QBDI 高速 Trace** | 指令级 trace + LZ4 实时压缩落盘（实测 2.21 亿指令/秒、2936 万条/7.45s） | 无（分析侧） | rustFrida（已有 stalker.rs/trace/） |
| 9 | **Zygote 注入与恢复** | 注入后恢复 Zygote 到干净状态，App 由干净 Zygote fork | zygote 状态检测 | rustFrida（loader.py 已有） |

**视频实测数据（性能基准）**：Stalker 2936 万指令 7.45s（221.6 Minsn/s）、stop 落盘 118ms、QBDI 同任务 ~21.7s（慢 ~108 倍）。

## 2. 现状盘点

### mkpms（本仓库）
- `kpms/wxshadow/` —— **#5 W^X Shadow 已完整实现**（~7500 行）：shadow 页状态机（NONE→SHADOW_X↔ORIGINAL↔STEPPING）、BRK/单步 handler hook、`do_page_fault`/`follow_page_pte` 隐藏、fork 保护（`dup_mmap`/`copy_process`）、TLB 三级 fallback、`prctl` 接口（`0x5758xxxx`）+ 静态客户端 `wxshadow_client`。
- `kpms/hide-maps/`、`kpms/anti-detect/`、`kpms/demo-*` —— 辅助模块。
- **缺口**：#1/#2/#3（幽灵内存分配、手动 PTE、自定义 Linker）均未实现。

### rustFrida（兄弟仓库）
- `agent/src/`：`stalker.rs`、`trace/`（transformer/ptrace_ops）、`exec_mem.rs`、`quickjs_loader.rs` —— #8 已有基础。
- `loader/`：`loader.py`/`loader.c` 注入器 —— #9 已有。
- `qbdi/`、`ldmonitor/`（含 ebpf）—— QBDI 集成与 linker 监控。
- **缺口**：#4（`gum_set_stealth_alloc` 对接幽灵内存）需本仓库 #1 提供内核接口后联动。

## 3. 实施路线图

### Phase 1（本批次，mkpms）：幽灵内存子系统 `ghostmem`
新建 KPM 模块，提供"内核侧无痕内存"能力，供用户态任意分配不可见 RWX 内存，并支撑后续 stealth trampoline / 自定义 Linker：

1. **ghostmem KPM 模块**：`__get_free_pages` 分配物理页 + 手动 PTE（复用 wxshadow 页表模式）+ `prctl` 接口（`PR_GHOSTMEM_*`）+ `exit_mmap` 生命周期钩子。
2. **ghostmem_client**：静态用户态工具，封装 alloc/free/read/write/信息查询。
3. **ghostlinker**：自定义 Linker v1 —— 在幽灵内存中手工映射最小 ELF：段映射 + RELA 重定位 + 符号解析回调 + init_array；TLS 明确为非目标（文档化）。
4. **文档**：本计划 + `docs/ghostmem.md`（原理/API/使用）+ `docs/ghostlinker.md`。

### Phase 2（rustFrida）：Stealth Trampolines 对接
`gum_set_stealth_alloc(stealth_code_alloc, stealth_code_free)` → 内部走 `prctl(PR_GHOSTMEM_ALLOC/FREE)`；`Interceptor`/`Stalker` 跳板全部落幽灵内存。前置：Phase 1 内核接口。

### Phase 3（rustFrida）：Trace 工具链加固
Stalker + LZ4 实时压缩落盘基准对齐视频数据；Chronos/AI MCP 导出格式。

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
