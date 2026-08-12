# android 无痕probe 无痕hook kpm demo

对任意地址设置断点，打印、修改寄存器，缺陷：无法对自读代码区域设置断点，同一页无法同时读取和执行。

包含模块：
- `wxshadow` — W^X Shadow 隐藏断点（shadow 页 + prctl 接口 + 客户端）
- `ghostmem` — VMA-Less 幽灵内存（手动 PTE，不登记 maps），stealth trampoline / 自定义 Linker 的内存底座（见 `docs/ghostmem.md`）
- `hide-maps` / `anti-detect` / `demo-*` — 辅助与示例

工具：`tools/ghostlinker/` — 自定义 Linker v1，在幽灵内存中手工映射 ELF（见 `docs/ghostlinker.md`）。

总计划与方案调研：`docs/PLAN.md`。

git config submodule.recurse true 自动更新submodule
# 使用方法
* 使用KernelPatch或者Apatch加载kpm模块，wxshadow_client -p pid -a address -r x0=9 -r可选的修改寄存器值，dmesg查看日志  
* 打开测试app，复制value函数的地址，设置断点测试，观察crc校验结果保持不变，修改寄存器改变value函数返回值  
* 复制crc32函数地址设置断点，app卡死，无法同时读取和执行  

<img width="1116" height="173" alt="c62acc29b9c00edf4d3a301c0aacd05a" src="https://github.com/user-attachments/assets/de6280d8-5093-46d3-9fed-db38355ae6f1" />

<img width="585" height="789" alt="d3d3b72fbf1c8d493d2098417def85f2" src="https://github.com/user-attachments/assets/e7be1d78-5d66-4ba7-8dfc-41c8c1f568a3" />

## 交流群

![wxshadow 微信群](image.png)
