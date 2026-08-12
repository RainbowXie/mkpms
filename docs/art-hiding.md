# ART 层特征隐蔽调研与设计（方案 #6）

> 目标：隐藏 ART（Android Runtime）Hook 产生的运行时特征，对抗企业壳的 ART 内部检测。
> 现状：rustFrida 已有 ART 控制器（`art_controller.rs`：ArtMethod 替换、OAT quick-method-header 隐藏、GC 同步钩子、`java._initArtController` + stealth 开关）；`hide_soinfo.c` 覆盖 soinfo 链摘除。本设计文档补充 entrypoint 扫描与幽灵内存跳板接入的细化方案。

## 1. 检测面分析

企业壳（茅台/个税/云闪付级）对 ART 层 Hook 的典型检测：

| 检测点 | 原理 | 我们的现状 |
|--------|------|-----------|
| **Method entry/exit hook 表** | ART 的 `ArtMethod` 被置 entrypoint 指向跳板（`quick_compiled_code_entrypoint` / `interpreter_entrypoint`） | 未做 |
| **Xposed 风格痕迹** | `ClassLoader.loadClass` 被 hook、`Method.invoke` 前后拦截 | 未做 |
| **ArtMethod 篡改** | `declaring_class`/`dex_method_index` 被改 | 未做 |
| **运行时内存特征** | 扫描 ART 内部结构（`art::Runtime` 单例、`ArtMethod` 数组）中的自定义地址 | 未做 |
| **JNI 全局引用异常** | 注入的 JNI 调用产生可疑 GlobalRef | 未做 |
| **soinfo 摘除** | 注入 so 从 linker 链表摘除 | ✅ `hide_soinfo.c` 已有 |

## 2. 设计原则

1. **无 Xposed 框架**：不引入 Xposed 的 `XposedBridge` 全局钩子（特征明显），改为**按需 entrypoint 替换**——只对目标方法改 `ArtMethod::entry_point_from_quick_compiled_code_`，指向幽灵内存中的跳板。
2. **跳板落幽灵内存**：entrypoint 跳板用方案 #1/#4（ghostmem + stealth trampoline）分配，`/proc/maps` 不可见、代码段无特征。
3. **恢复原状**：Hook 撤销时还原原始 entrypoint，不留 ArtMethod 残留（对齐视频"干净状态"理念）。
4. **不碰 libart 头文件偏移**：`ArtMethod` 布局因版本而异——运行期**特征扫描**推导 entrypoint 偏移（复用 `hide_soinfo.c` 的"版本无关机器码分析"思路）。

## 3. 技术方案（rustFrida 侧）

### 3.1 ArtMethod entrypoint 定位（版本无关）

```
1. jclass + GetMethodID 拿 jobject (ArtMethod*)
2. 读 ArtMethod 头部 16 字节 = declaring_class_ + access_flags_ + dex_code_item_offset_ + dex_method_index_
   （4.4-15 稳定布局）
3. entrypoint 偏移 = 在对象后部扫描"指向代码段且可执行"的指针
   —— 候选须满足 is_kva 且落在某个已映射可执行段内
4. 校验：修改后调用该方法，观察是否进入我们的跳板（回环验证）
```

### 3.2 Hook 流程

```
JNI (native)                       ghostmem 模块
  │ GetMethodID(method)
  ├─ 保存原始 entrypoint            ┌─ stealth_alloc() 分配跳板页
  ├─ 写跳板到幽灵内存               │   (PR_GHOSTMEM_ALLOC)
  ├─ entrypoint = 跳板              └─ 原始指令 + 跳回逻辑
  └─ 跳板执行 onEnter/onLeave       ──► 调用宿主逻辑（QuickJS callback）
```

### 3.3 特征隐蔽点

- 跳板地址在幽灵内存 → ART 内部扫描不到代码段新增
- 不修改 `ArtMethod` 的 dex 字段 → dex_method_index/declaring_class 保持原值
- Hook 撤销完整还原 → 无"曾 hook"状态残留

## 4. 验收标准（真机）

- [ ] 对目标方法 hook 后，`Runtime::GetRuntime()` 枚举全部 ArtMethod 无跳板地址
- [ ] 撤销 hook 后 entrypoint 字节级还原
- [ ] 高防护 App（如个税）hook Java 方法不闪退、无风险提示
- [ ] 主/子线程 hook/撤销并发安全（entrypoint 写用原子）

## 5. 风险与限制

- **ART 版本碎片**：Android 7-16 ArtMethod 布局有差异，entrypoint 扫描需真机矩阵验证
- **entrypoint 写竞态**：方法正在执行时改 entrypoint 需 `art::ScopedSuspendAll` 或接受极小竞态窗口（记录）
- **隐藏 API 限制**：`GetMethodID`/反射调用在新 SDK 受 hidden-api 限制，需绕过（后续独立项）
- **无真机**：本设计仅静态推导，实现后需 APatch 环境验证

## 6. 现状与归属

| 项 | 状态 |
|----|------|
| soinfo 摘除（dl_iterate_phdr 对抗） | ✅ rustFrida `hide_soinfo.c` |
| ART 控制器（ArtMethod 替换/OAT 隐藏/GC 钩子） | ✅ 已有 `quickjs-hook/src/jsapi/java/art_controller.rs`（788 行） |
| `java._initArtController` + stealth 开关 | ✅ 已有（`java` JS 对象） |
| 跳板落幽灵内存 | ✅ 前置就绪（ghostmem + stealth callbacks） |
| 真机验证 | ⏳ 需 APatch 环境 |

## 7. 与幽灵内存的结合点（#4 ↔ #6）

`art_controller.rs` 的 replacement ArtMethod 当前用 C 堆（`libc::free` 释放）。与方案 #4 的结合：

- **理论收益**：replacement 落入幽灵内存 → ART 内部扫描/内存特征检查找不到堆上的替换方法
- **关键风险（GC 可达性）**：ArtMethod 含 GcRoot（declaring_class_），ART 的 GC 需要从根集可达这些对象。纯 VMA-Less 页不参与 GC 扫描 → 可能被回收或漏标。**结论：不做盲改**；若推进，需先在真机验证 GC 对幽灵页 ArtMethod 的处理，或用"幽灵内存 + 显式 GlobalRef 保根"组合。
- **已落地替代**：OAT quick-method-header 隐藏 + JIT 缓存失效已让替换对 ART 内部不可见；堆分配本身在当前检测模型下未被枚举（真机验证项）。

## 关联

- `docs/ghostmem.md`、`docs/stealth-trampolines.md` —— 跳板内存前置
- `docs/PLAN.md` —— 路线图（Phase 4 候选）
