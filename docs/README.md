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

- mkpms：`ctest` — `ghostmem_abi_test`（12 断言）、`ghostmem_client_overlap_test`（8 断言）；`tools/ghostlinker` — `ghostlinker_test`（12 断言）
- rustFrida：`lz4_block`（7 测试含 lz4_flex 互操作）、`ghostmem.rs`（5 测试）、`trace-decoder`（多块 roundtrip + 端到端）
- **回归基线（Iteration 21 终检）**：跨套件全部通过 —— mkpms 2/2、ghostlinker 1/1、rustFrida 10/10、trace-decoder 1/1（共 24 项断言 + 2 端到端）
- **内核语法核查（Iteration 27）**：`scripts/kernel_syntax_check.sh` 用真实 KernelPatch 框架头验证 8 个内核源文件（ghostmem 3 + wxshadow 5）全部通过
- **最终回归矩阵（Iteration 32）**：mkpms ctest 5/5（60 断言）、ghostlinker 1/1（12）、rustFrida ghostmem 5/5、lz4_block 5/5（含标准库互操作）、trace-decoder 8/8（真实编码器 e2e）、内核语法 8/8 —— **全绿**
- **执行级 harness**：pgtable 26 断言（map/unmap/reclaim/权限/共享表/PTE 拷贝）+ core 15 断言（空洞查找/alloc/free/RWX 归一化）——共抓出 3 个语法检查发现不了的真 bug（PTE_USER 缺失、prot==0 语义、表页残留）

## 关联仓库

- `../rustFrida` — Frida Gum Rust 绑定 + 注入/trace（Phase 2/3/4 落点）
- `../solution/` — 视频文案与方案调研原始资料
