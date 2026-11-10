# 0x101 排他创建静态复核（2026-11-04）

**新结论：C3.11 的 0x101 在这里不是普通 open-or-create，而是 create-new / 既存失败语义。** 可把该地址／flag 组合列为有静态依据的 native exclusive-create 实现；不能据此宣称已做过设备碰撞文件不变或并发竞态的运行验收。本轮只读指定 stock MAIN，用本地 Capstone；没有执行 native 函数，没有 QEMU，也未改固件。

完整链：

1. `0x402daa4c(path,flags,attr)` 把 path/flags/attr 写入 request `+0x20/+0x28/+0x2c`，op=4 写 `+0x10c`；`0x402da88c` 同步提交。服务分发表 `0x41f180e8[4] = 0x402d9b94`，此处分发原参数到 `0x402dd4f8`。因此旧 Live 使用的 file API 确实经该 wrapper，而不是另一套未知创建语义。
2. `0x402dd4f8` 在 `0x402dd524` 用 `0xa02e81dd8` 进入互斥，`0x402dd534 → 0x408b6878 → 0x408b6880` 完成整个 open/create，`0x402dd540` 才退出互斥。`0x408b6878` 设第四参数0，故 lookup 的特殊绕过参数 `w27` 为0。另一个直接 caller `0x402dd988` 也持相同互斥（但明确使用0x301）。
3. flags转换：0x101 在 `0x408b6968…6984` 得写访问 internal=2；不含 `0x208`，`0x408b6a04…6a0c` 加0x10，故 descriptor+2=0x12；`0x408b8648` 再加0x80。0x301 则在 `0x408b69d8…6a00` 得 internal=0x26（含truncate bit2及create-or-open bit5），与0x101不同。
4. `0x408af168` 先用 create=0 查找目录。根目录 ExFAT 分支 `0x408af3fc…410` 调 `0x408afecc(...,w3=0,...)`；有父目录的最终分支 `0x408af780…798` 调 `0x408b0c9c(...,w4=0,...)`。后者在 `0x408b1144` 根据保存的 create 参数分开查找与创建：0进入 `0x408b1158`，识别0x85 entry、读取匹配；1才进入 `0x408b116c` 空槽创建路径。查找成功以0返回，仅把目录信息填入候选 file descriptor（`0x408b19e4…1a0c`）。
5. **既存成功路径**：根目录 `0x408af56c → 0x408af574`；有父目录 `0x408af7c8 → 0x408af8c8 → 0x408af574`。`0x408af57c` 检查 internal bit4；0x101必跳 `0x408af5b4`，释放临时 path 对象后返回 **0x80000002**。该临时释放 `0x408be2a0` 只清池槽的占用字，不是文件删除。错误从 `0x408b866c` 返回，无法到达成功后的注册/截短。
6. **缺失路径**：查找返回0x80000010时，`0x408af2f8` 或 `0x408af7d0` 检查0x30，然后把 create 参数置1再调用目录函数；创建和第一次查找均在上述同一互斥区内。不存在应用层“查存在→解锁→创建”的竞态窗口。
7. 截短代码 `0x408b6abc` 只在 internal bit2 时执行，释放已有数据链并清长度；0x101 bit2=0，且既存文件此前已失败，**不会走这段**。0x301可以走，绝不能替换使用。

边界：这证明已分析原生 wrapper 路径内的拒绝既存和串行语义，足以修正之前“0x101没有证据”的结论；未遍历所有可能 indirect/绕过底层的恶意或第三方写者，也未验证拔卡/断电耐久性。目录查找可能有 cache/descriptor 更新，不应声称“完全零内存写”；对目标既存文件没有进入 create/truncate/write 路径。下轮运行仍宜预置不同内容 LIVE 文件，验证 open错误、长度/哈希不变及同时创建碰撞。

收据：`fs-exclusive-analysis.txt`（0x408af168完整主路）、`fs-exclusive-exfat.txt`（目录查找/创建分支）、`fs-exclusive-routing.txt`、`fs-exclusive-open-request.txt`（请求/分发表）、`fs-exclusive-callers.txt`。上层 flags 和锁的原始区间另见父目录 `fs-open-disassembly.txt`、`fs-create-disassembly.txt`。所有输入为 stock SHA256 `2b43a31c9de838f84b12416de4348a7004e65be07f853ec56ec2b10f3a8e3853`。
