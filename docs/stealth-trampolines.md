# Stealth Trampolines — 无痕跳板对接设计（Phase 2）

视频方案 #4 的落地设计：让 Frida Gum 的 `Interceptor.attach()` 与 Stalker 生成的代码跳板自动落在**幽灵内存**（ghostmem 模块），使跳板不进 `/proc/pid/maps`、不产生代码段特征。本仓库提供内核侧接口（已完成），本设计文档定义 rustFrida 侧的对接方式。

## 原理

Frida Gum 的跳板分配器默认用 `mmap`/`Memory.alloc`（有 VMA → 可被扫描）。Gum 暴露钩子：

```c
/* gum/gum.h — Frida Gum API（已文档化） */
void gum_set_stealth_alloc(gum_stealth_alloc_t alloc, gum_stealth_free_t free);
```

`gum_stealth_alloc_t` 签名（frida-gum）：

```c
typedef gpointer (*gum_stealth_alloc_t)(gsize size);
typedef void     (*gum_stealth_free_t)(gpointer mem);
```

启用后，Gum 内部所有代码跳板（Interceptor trampoline、Stalker 变换后的代码块）都经此分配。我们要做的就是把这个分配器指向幽灵内存。

## 内核侧现状（已完成，本仓库）

`kpms/ghostmem/` 提供：

```c
#define PR_GHOSTMEM_ALLOC  0x47474d01  /* prctl(opt, pid=0, nr_pages, prot, 0) -> VA */
#define PR_GHOSTMEM_FREE   0x47474d02  /* prctl(opt, pid=0, va, 0, 0) */
```

- `pid` = 目标进程（pid=0 为当前进程）；INFO 统计目标 pid，缓冲在调用进程
- prot 默认 RWX（跳板需可执行）；单次上限 64 页（256KB）
- 返回 VA 是 VMA-Less 的，`/proc/pid/maps` 不可见

## rustFrida 侧对接方案

rustFrida 已有 `agent/`（QuickJS 运行时 + `gumlibc.rs` + `stalker.rs`），Phase 2 改动集中在：

1. **新模块 `agent/src/ghostmem.rs`**：封装 prctl 调用

```rust
// 签名对齐 frida-gum: (usize) -> *mut c_void
pub unsafe extern "C" fn stealth_alloc(size: usize) -> *mut c_void {
    let pages = (size + 4095) / 4096;
    let ret = prctl(PR_GHOSTMEM_ALLOC, 0, pages, PROT_RWX, 0);
    if ret < 0 { std::ptr::null_mut() } else { ret as *mut c_void }
}
pub unsafe extern "C" fn stealth_free(mem: *mut c_void) { ... }
```

2. **初始化接入**（`agent/src/lib.rs` 或 loader 注入后）：

```c
// C 侧一行：
gum_set_stealth_alloc(stealth_alloc, stealth_free);
```

3. **验证**（真机，APatch 环境）：
   - `Interceptor.attach(...)` 后 `cat /proc/<pid>/maps` 无新增 RWX 匿名段
   - `dmesg | grep ghostmem` 显示 alloc 日志

## 风险与回退

- **ghostmem 模块未加载**：`prctl` 返回错误 → `stealth_alloc` 返回 NULL → Gum 回退默认分配器（需确认 Gum 对 NULL 的处理；若硬崩则改为内部 mmap 回退，见 ASSUMPTIONS.md）
- **64 页上限**：Stalker 大块变换可能超过；必要时提升 `GHOSTMEM_MAX_PAGES` 或分块
- **幽灵内存无 VMA**：Gum 内部若对跳板做 `mprotect`（改权限）会失败（无 VMA 可改）——需在文档中标注"v1 跳板分配后不再改权限"

## 状态

| 步骤 | 状态 |
|------|------|
| 内核接口（ghostmem 模块） | ✅ 本仓库已完成 |
| prctl 常量一致性 | ✅ 已核查（三处对齐） |
| rustFrida `ghostmem.rs` 分配器 + `stealth_alloc/free` 回调 | ✅ 已实现（prctl VMA-Less + 5 tests） |
| `gum_set_stealth_alloc` 注册 | ⏳ 依赖 frida-gum 17.x（当前锁定 16.7.18 无此 API）+ agent 构建链 |
| 真机验证 | ⏳ 需 APatch 环境 |

## 关联

- `docs/ghostmem.md` — 幽灵内存模块使用
- `docs/PLAN.md` — 三阶段路线图
- `ASSUMPTIONS.md` — 环境与语义假设
