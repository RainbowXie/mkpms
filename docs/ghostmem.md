# GhostMem — VMA-Less 幽灵内存模块

内核侧无痕内存底座：为指定进程分配**不登记在 `/proc/<pid>/maps`** 的 RWX 内存（视频方案 #1/#2 的落地）。用户态通过 `prctl` 接口使用，是后续 stealth trampoline（`gum_set_stealth_alloc`）与 ghostlinker（自定义 Linker）的内存来源。

## 原理

普通 `mmap` 会创建 `vm_area_struct`（VMA），`/proc/<pid>/maps` 就是遍历 VMA 链表生成的——所以任何 mmap 出来的内存都会被壳扫描到。ghostmem 反其道而行：

1. **不创建 VMA**：`__get_free_pages()` 直接分配物理页；
2. **手动建 PTE**：从 `mm->pgd` 逐级下探（PGD→PUD→PMD→PTE），中间表缺失时分配表页，在目标用户地址写入页表项；
3. **地址选在 VMA 空洞**：用 `find_vma` 从 4GB 起向上扫描，跳过所有已占用区间，落在堆顶与 mmap 区之间的空洞里。

结果：进程能正常读写执行该地址（页表存在、权限正确），但 `/proc/pid/maps`（VMA 视图）完全看不到它。

```
用户态                       内核态 (ghostmem.kpm)
  prctl(ALLOC,pid,n)  ──►  find_vma 找空洞
                           __get_free_pages 分配物理页
                           手动写 PTE (USER|AF|NG|RWX)
                           ──► 返回用户 VA
  读写执行 VA        ──►  直接命中（无 VMA 参与）
```

## prctl 接口

| 选项 | 调用 | 说明 |
|------|------|------|
| `PR_GHOSTMEM_ALLOC (0x47474d01)` | `prctl(opt, pid, nr_pages, prot, 0)` | 分配，返回用户 VA；`prot` 用 `PROT_READ/WRITE/EXEC` 位（0=默认 RWX），最大 64 页 |
| `PR_GHOSTMEM_FREE (0x47474d02)` | `prctl(opt, pid, va, 0, 0)` | 释放整块（须传块基址），未登记地址返回 `-EINVAL` |
| `PR_GHOSTMEM_INFO (0x47474d03)` | `prctl(opt, pid, &stats, sizeof, 0)` | 返回 `{nr_blocks, nr_pages}` 统计 |

`pid=0` 表示当前进程；非法 pid 返回 `-ESRCH`。

## 使用

```bash
# 部署（需 APatch/KernelSU + aarch64 交叉编译）
adb push build/kpms/ghostmem/ghostmem.kpm /data/local/tmp/
adb push build/kpms/ghostmem/ghostmem_client /data/local/tmp/
kpatch module load /data/local/tmp/ghostmem.kpm

# 分配 16 页 RWX 幽灵内存给 pid 1234
./ghostmem_client -p 1234 -a 16            # -> 0x100000000
# 校验 maps 不可见
./ghostmem_client -p 1234 -c 0x100000000   # -> INVISIBLE in maps (ok)
# 写入/读取/释放
./ghostmem_client -p 1234 -w 0x100000000 9090
./ghostmem_client -p 1234 -r 0x100000000 2
./ghostmem_client -p 1234 -f 0x100000000
```

## 生命周期

- **进程退出**：`exit_mmap` 钩子在 mm 销毁前释放该 mm 全部幽灵块（物理页 + 空表页回收）；
- **模块卸载**：先摘 `prctl` 钩子阻止新操作，释放全部遗留块，最后摘 `exit_mmap`；
- **空表回收**：解除映射后逐 2MB 组检查 PTE/PMD/PUD 表页是否全空，全空才释放并上溯清槽位——多块共享表页时不会误释放。

## 已知限制

- **VMA-less 的固有风险**：地址没有 VMA，若进程后续恰好 mmap 到该区间，内核可能覆盖我们的 PTE（幽灵页"显形"）。扫描起点 4GB 远低于 Android 典型 mmap_base，实际冲突概率低；
- **`/proc/pid/pagemap` 仍可见 PFN**（GUP 层隐藏是后续工作，wxshadow 的 `follow_page_pte` 钩子可复用）；
- **fork 不继承**：幽灵映射没有 VMA，子进程全新 mm 不含这些 PTE（符合预期）。

## 文件

| 文件 | 说明 |
|------|------|
| `kpms/ghostmem/ghostmem.c` | 核心：块链表、prctl 处理、生命周期、空洞查找 |
| `kpms/ghostmem/ghostmem_pgtable.c` | 手动 PTE 构建/解除、TLB 广播失效、空表回收 |
| `kpms/ghostmem/ghostmem_scan.c` | vmlinux 符号解析 |
| `kpms/ghostmem/ghostmem_client.c` | 用户态静态工具 |
| `docs/ghostlinker.md` | 自定义 Linker（消费本模块内存） |

## 后续（见 docs/PLAN.md）

- Phase 2（rustFrida）：`gum_set_stealth_alloc(stealth_code_alloc, stealth_code_free)` → 内部调 `PR_GHOSTMEM_ALLOC/FREE`，`Interceptor`/`Stalker` 跳板全部落幽灵内存；
- `follow_page_pte` 钩子隐藏 GUP/PFNs。
