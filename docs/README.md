# docs

mkpms / SeamlessHook 项目的技术文档索引。

## 总览

| 文档 | 内容 | 对应方案 |
|------|------|---------|
| [PLAN.md](PLAN.md) | 9 个无痕 hook 方案调研 + 现状盘点 + 三阶段路线图 + 状态总表 | 全部 |
| [ghostmem.md](ghostmem.md) | VMA-Less 幽灵内存模块：原理 / prctl API / 使用 / 生命周期 / 限制 | #1 #2 |
| [ghostlinker.md](ghostlinker.md) | 自定义 Linker v1：幽灵内存中 ELF 映射 / 重定位 / init_array | #3 |
| [stealth-trampolines.md](stealth-trampolines.md) | Frida Gum 无痕跳板对接（`gum_set_stealth_alloc`）：设计 / 状态 | #4 |
| [art-hiding.md](art-hiding.md) | ART 层特征隐蔽：检测面 / 设计 / 与 #4 结合点分析 | #6 |
| [coexistence.md](coexistence.md) | wxshadow × ghostmem 协同：组合用法 / 接口隔离 / 卸载顺序 | #5+#1 |

## 方案状态（9 项）

- ✅ **已实现**：#1/#2 幽灵内存（ghostmem KPM）、#3 自定义 Linker（ghostlinker）、#5 W^X Shadow（wxshadow）、#6 ART 隐蔽（art_controller.rs）、#9 Zygote 注入（loader.py）
- 🕐 **进行中**：#4 stealth trampolines（ghostmem.rs 分配器 ✅，`gum_set_stealth_alloc` 注册待 frida-gum 17.x）、#7 Native 无痕 Hook（依赖 #4）、#8 Stalker trace（LZ4 全链路 ✅，真机基准待验）
- ⏳ **待真机**：全部内核路径（ghostmem/wxshadow）需 APatch 环境验证

## 测试

```bash
# 跨仓库一键全测试（mkpms ctest + 内核语法 + rustFrida host 侧）
./scripts/run_all_tests.sh /mnt/data/Work/Projects/KernelPatch

# 仅 mkpms（host 模式：KPM 模块自动跳过，host 测试/客户端照常）
cmake -S . -B build -DKP_DIR=$PWD
cmake --build build
ctest --test-dir build
# => 19 套件全部通过（无需交叉编译器）
```

- host 模式下 `add_kpm_module` 自动跳过 KPM 目标（非 aarch64 编译器）；设备构建加 `-DCMAKE_C_COMPILER=aarch64-linux-gnu-gcc` 即恢复。

- mkpms：根构建 `ctest` 统一覆盖 22 套件 — `ghostmem_abi_test`（17）、`ghostmem_client_overlap_test`（14）、`ghostmem_args_test`（8）、`ghostmem_pgtable_harness`（37）、`ghostmem_core_harness`（26）、`ghostlinker_test`（21）、`ghostlinker_cli_run`（1）、`ghostlinker_offbase_test`（6）、`ghostlinker_bss_test`（3）、`ghostlinker_resolver_test`（4）、`ghostmem_client_e2e_test`（7+1 skip）、`wxshadow_abi_test`（8）、`wxshadow_client_e2e_test`（5）、`wxshadow_pte_state_test`（15）、`wxshadow_brk_esr_test`（15）、`wxshadow_tlb_test`（6）、`wxshadow_next_task_test`（4）、`wxshadow_addr_test`（11）、`vma_offset_test`（7）、`wxshadow_vma_offset_test`（6）、`wxshadow_comm_offset_test`（6）、`wxshadow_tasks_offset_test`（6）
- rustFrida：仓库内 `cargo test` 直跑 — `host-tests`（10 测试：ghostmem 5 + lz4 5 含标准库互操作）、`trace-decoder`（8 测试：多块 roundtrip + 真实编码器 e2e + 性能验收）——均无需 NDK（独立 host target 配置）
- **回归基线（Iteration 21 终检）**：跨套件全部通过 —— mkpms 2/2、ghostlinker 1/1、rustFrida 10/10、trace-decoder 1/1（共 24 项断言 + 2 端到端）
- **内核语法核查（Iteration 27）**：`scripts/kernel_syntax_check.sh` 用真实 KernelPatch 框架头验证 8 个内核源文件（ghostmem 3 + wxshadow 5）全部通过
- **最终回归矩阵（Iteration 32）**：mkpms ctest 5/5（60 断言）、ghostlinker 1/1（12）、rustFrida ghostmem 5/5、lz4_block 5/5（含标准库互操作）、trace-decoder 8/8（真实编码器 e2e）、内核语法 8/8 —— **全绿**
- **执行级 harness**：pgtable 26 断言（map/unmap/reclaim/权限/共享表/PTE 拷贝）+ core 15 断言（空洞查找/alloc/free/RWX 归一化）——共抓出 3 个语法检查发现不了的真 bug（PTE_USER 缺失、prot==0 语义、表页残留）

## 关联仓库

- `../rustFrida` — Frida Gum Rust 绑定 + 注入/trace（Phase 2/3/4 落点）
- `../solution/` — 视频文案与方案调研原始资料
