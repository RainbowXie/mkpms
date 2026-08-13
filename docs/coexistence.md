# wxshadow × ghostmem 协同使用

两个 KPM 模块互补实现视频方案的"断点隐藏 + 内存隐藏"组合。本页说明同时加载时的使用方式与接口隔离。

## 模块分工

| 模块 | 能力 | prctl 族 | 适用场景 |
|------|------|---------|---------|
| `wxshadow` | W^X Shadow **断点**：shadow 页复制代码、断点写 shadow、读取回原始页 | `0x57580001..08` | 在**已有映射页**上设置隐藏断点 |
| `ghostmem` | VMA-Less **幽灵内存**：无 VMA 的 RWX 分配 | `0x47474d01..03` | 分配**全新不可见**内存（跳板/载荷） |

## 组合用法

视频流程的典型组合：

```
1. ghostmem_client -p <pid> -a 16            # 分配幽灵内存放跳板
2. (ghostlinker) 在幽灵内存中映射/重定位载荷
3. wxshadow_client -p <pid> -a <orig_addr>    # 在原代码页设断点
   -r x0=...                                  # 修改寄存器
```

- **断点目标**必须是已映射页（wxshadow 依赖现有 PTE）；
- **跳板/载荷**用幽灵内存（ghostmem 新建 PTE，不登记 maps）；
- 两者互不干扰：prctl 常量族（`0x5758` vs `0x4747`）在各自模块的 `prctl_before` handler 中过滤，**不冲突**。

## 接口隔离（并发安全）

- 两个模块各自 `hook_syscalln(__NR_prctl, 5, ...)` 挂 prctl；KP 框架的 hook 链支持多个 before handler 串联
- 每个 handler 检查自己的 option 范围，非本族直接 return（不设 `skip_origin`）
- `ghostmem` 与 `wxshadow` 的 `exit_mmap` 钩子各自清理自己的块/页，互不感知

## 已知边界

- **表页分配能力未跨模块复用**：`ghostmem_get_or_create_pte`（可分配缺失的中间表页）是 wxshadow 没有的能力；wxshadow 只操作已有 PTE。KPM 模块独立加载，跨模块函数调用需各自解析符号——当前不共享（后续可在 wxshadow 侧复制该模式）。
- **卸载顺序**：先卸载 ghostmem 再卸载 wxshadow 或反之均可；各自 exit 清理自身遗留块。

## 验证建议（真机）

```bash
kpatch module load wxshadow.kpm
kpatch module load ghostmem.kpm
ghostmem_client -p <pid> -n 16 && echo "ghost OK"        # 幽灵内存不可见
wxshadow_client -p <pid> -a 0x7b5c001234                  # 断点生效
dmesg | grep -E "wxshadow|ghostmem"                       # 双模块日志
```

## rustFrida stealth hook 接线（方案 #5 全链路）

`hook(ptr, cb, stealth=true)` 的完整调用链（Iteration 46-48 验证）：

```
JS hook(ptr, cb, true)
  → hook_replace(addr, cb, addr, stealth=1)
  → patch_target(target, thunk, stealth=1)
  → wxshadow_patch(addr, buf, len)
      prctl(PR_WXSHADOW_PATCH 0x57580006, pid=0→getpid(), addr, buf, len)
      ├─ 成功：内核 shadow 页 one-step（copy + activate --x）
      └─ 失败（PMD 2MB 大页）：pmd_split_cow
           ├─ 整 VMA mprotect(rwx) —— 不分裂 VMA（防 V-OS maps 扫描）
           ├─ 自我写回触发 COW 分裂 PMD（值不变，安全）
           └─ 恢复 r-x，重试 PATCH
```

- prctl 常量跨仓库对齐已由 `wxshadow_abi_test`（8 断言）自动锁定。
- 设计要点：整 VMA mprotect 避免 `/proc/maps` 出现 VMA 分裂（V-OS 检测面）。

## prctl 常量冲突审计

| 族 | 范围 | 说明 |
|----|------|------|
| 内核标准 | `0x41555856` / `0x53564d41` / `0x59616d61` | `PR_GET_AUXV` / `PR_SET_VMA` / `PR_SET_PTRACER`（linux/prctl.h） |
| wxshadow | `0x57580001..08` | WX 前缀 |
| ghostmem | `0x47474d01..03` | GM 前缀 |

- 三族**互不重叠**；各 prctl handler 按自身范围过滤，范围外直接 return（不设 skip_origin），不影响内核/其他模块处理。
- 审计依据：`/usr/include/linux/prctl.h`（标准）逐项核对；新增 prctl 选项时须复查本表（Iteration 40 审计）。

## 关联

- `docs/ghostmem.md`、`docs/stealth-trampolines.md`
- wxshadow 源码：`kpms/wxshadow/`（CLAUDE.md 有完整说明）
