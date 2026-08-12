# GhostLinker — 自定义 Linker v1（幽灵内存 ELF 加载器）

视频方案 #3 的落地：在幽灵内存（ghostmem 模块提供）中**手工映射 SO**，绕过系统 linker/soinfo，使加固壳的「soinfo 列表扫描 / linker 符号比对」找不到我们。

## 原理

系统加载 SO 时，linker 会：映射段 → 建立 `soinfo` 并登记进全局链表 → 重定位。壳扫描的就是 `soinfo` 与内存特征。ghostlinker 把前三步搬到幽灵内存里自己完成：

1. **段映射**：解析 ELF64 的 `PT_LOAD`，按 `p_vaddr/p_offset/p_filesz/p_memsz` 拷贝到幽灵内存（BSS 清零）；
2. **重定位**：读 `PT_DYNAMIC` 的 `DT_RELA/DT_RELACOUNT`，应用 RELA 重定位；
3. **符号解析**：本模块内部符号直接 `base+st_value`，外部符号走宿主回调（默认拒绝猜测）；
4. **init_array**：收集并（可选）调用构造函数。

```
ghostlinker_cli mini.so
  ├─ default_alloc(): prctl(PR_GHOSTMEM_ALLOC, pid=0, ...)  ← 幽灵内存
  │    └─ 失败回退 mmap（本机测试）
  ├─ compute_layout(): PT_LOAD 覆盖范围 -> (base, size)
  ├─ load_segments():  拷贝段 + 清 BSS
  ├─ process_dynamic(): RELA 重定位 + 收集 init_array
  └─ 打印 base/init_array，可选 --run 调用构造
```

## 支持的重定位

| 类型 | 语义 | 说明 |
|------|------|------|
| `R_AARCH64_RELATIVE` | B + A | 无符号，最常用 |
| `R_AARCH64_ABS64` | S + A | |
| `R_AARCH64_GLOB_DAT` / `R_AARCH64_JUMP_SLOT` | S | GOT 填充 |
| `R_X86_64_*`（同语义 4 种） | — | 仅宿主测试用，目标平台是 ARM64 |

其余类型（TLS / IFUNC / 跳转类）显式返回 `-ENOTSUP`，**不做猜测**。

## 构建与验证

```bash
# ghostlinker 是 host 侧工具，独立于 KPM 构建
cmake -S tools/ghostlinker -B build/ghostlinker && make -C build/ghostlinker

# 本机验证（mmap 回退路径）：
gcc -shared -fPIC -o /tmp/mini.so -x c - <<'EOF'
int g = 42; int *p = &g;
__attribute__((constructor)) static void ctor(void) {}
EOF
./build/ghostlinker/ghostlinker_cli /tmp/mini.so
# -> loaded: base=0x... size=20480 init_array=0x... nr_init=1
```

真机验证（ghostmem 模块已加载时）：分配来源自动切换到幽灵内存，加载后 `cat /proc/<pid>/maps | grep <base>` 应无输出。

## 库式 API

```c
struct gh_linker_cb cb = {
    .resolve = my_resolver,   /* 外部符号解析回调；NULL = 禁止外部符号 */
    .udata   = ctx,
    .alloc   = NULL,          /* NULL = 默认（幽灵内存→mmap 回退） */
};
struct gh_linker_result res;
int ret = gh_link_elf(elf_buf, elf_size, &cb, &res);
/* res.base / res.size / res.init_array / res.nr_init */
gh_link_free(&res);
```

## 已知限制（v1 明确非目标）

- **TLS**（`DT_TLS`/`DT_TLSDESC`）不处理——仅支持无 TLS 依赖的 payload（stub/补丁代码）；
- **IFUNC**（`R_AARCH64_IRELATIVE`）不支持；
- 外部符号解析依赖宿主回调，不含全局作用域查找；
- 不建 `soinfo`——主动调用需宿主自行记录导出符号（与"零 soinfo 特征"目标一致，属设计取舍而非缺陷）。

## 后续

- v2：TLS 初始化（对齐视频"修复 TLS bug"）、`DT_INIT` 入口、导出符号表 API；
- Phase 2（rustFrida）：与 `gum_set_stealth_alloc` 对接后，`Interceptor.attach` 的跳板可直接落在 ghostlinker 提供的幽灵内存里。
