# REVIEW3 验证器修复记录

2026-11-09。修改限于 src-fix35/recorder/ 和 recorder-build/recorder/ 中的 verify_link.py、test_verify_link.py，以及本记录。未改 recorder 运行代码、hooks、patch。没有 USB、刷写、相机操作或 QEMU。

## 检查方式

- 删除候选 ELF 函数范围和线性分支目标收集。固定入口指 OLD 表中的外部 patch PC；先核对该处实际 B/BL，再从其实际目的地址执行控制流证明。符号只做 patch 一致性校验，不提供证明入口或范围。
- 代码路径限制固定为 `[0x401e2000, 0x401fe000)`，还必须属于 ELF/MAIN 已核对一致的已映射字节。每路径最多 256 步、每次证明最多 8192 个状态，候选 st_size 不改变这些限制。
- 直接跳转跟随实际目的地址；间接跳转必须有可靠的整数常量。条件跳转两边都必须成功，不采用“存在一条成功路径”作为通过条件。提前返回、未知分支、循环、越界、未知指令均 FAIL。
- BL/BLR 子调用实际进入，不假设它会返回。wrapper 在到达 stock 前遇到 RET 即 FAIL；子调用的 RET 仅在寄存器返回地址等于已记录的实际调用返回地址时允许，随后继续证明调用者路径。这一例外用于真实 call/return 控制流，不是跳过 callee。
- 保留六个旧 continuation 和六个旧转发符号的 SHA 固定基线分支证据；为 open/finalize/event 的绝对 stock 地址增加基线固定指令路径证据。finalize 的基线尾段仅用于确定 stock 地址，不被当作候选 wrapper 入口。
- 对不支持的指令、非栈写入、SIMD/lane 常量传播保守拒绝，不用猜测填补证明。

## 实测通过项

运行：

```sh
.recorder-venv/bin/python src-fix35/recorder/test_verify_link.py recorder-build
```

最终输出：

```
PASS: synthetic fixture + 3 CFG positives and 43 negative cases; real compiler open/event prefixes and finalize suffix; NOT a real full payload link
```

- 原有 synthetic 正例 PASS（20 个 patch sites）。开始修复前也运行了原脚本：原有正例与 22 个负例通过。
- 原有全部 22 个负例继续拒绝：7 个基础破坏、12 个符号/分支同时错误、3 个绝对转发破坏。
- 新增 9 个旧 wrapper 的自洽 RET 入口负例全部由 `old forwarding proof failed` 拒绝，不是因为 ELF/MAIN 不一致或后续 hooks 对比失败。
- open 精确复现 REVIEW3：入口 `0x401e3c34` 为 RET，patch site `0x406cd468` 同步跳转，st_size=`0x20` 覆盖 `0x401e3c50` 的正确 stock 转发。其余八个 wrapper 各有 RET 后包含正确 stock 分支的独立探针。
- 额外 12 个自洽 ELF/MAIN 对抗负例全部拒绝：跳过正确分支后 RET、条件一侧 RET、循环、未知 BR、固定范围外跳转、callee 循环、破坏 LR、W 寄存器覆盖、未知 RET 寄存器、BRK、SIMD lane 混淆、常量被未知值覆盖。
- 新增 3 个完整 verify 正例：跨块直接跳转、条件两边均转发、常量间接转发。刻意把 st_size 设为 1，验证函数大小不参与证明。
- 保留真实编译对象的三个绝对目标正例，改为固定指令路径检查：open/event 为入口至首次 stock 调用；finalize 仅为终端转发尾段。它们不是完整 runtime 对象 CFG 验证。
- 另测 256 条 NOP 超预算，得到 `forward path limit`，无递归异常。两份脚本内容一致。
- 测试 ELF/MAIN 在临时目录生成，并在结束或失败时清理；未生成真实链接/可刷写交付物。

## 自我对抗：确实发现并修复了第一版的新漏检

第一版路径检查仍把 SIMD MOV 误当成标量 MOV。构造的自洽 ELF 保持 patch、符号和 ELF/MAIN 一致，实际路径执行：

```asm
mov  w8, #0xb320
movk w8, #0x4085, lsl #16
mov  v0.d[0], x8
mov  x8, v0.d[1]
br   x8
```

lane 1 未知，不能证明跳到 stock。第一版完整 verify() 却返回 PASS，回归测试当场报 `accepted self-consistent ELF/MAIN, SIMD lane alias smuggling`。这不是只测了一个辅助函数。

随后将常量传播限定到明确支持的整数寄存器；向量/lane 指令在进入模型前即拒绝。最终该用例 FAIL：`unsupported register/lane`，PC=`0x401e3d38`。修复后再构造 W 覆盖、未知返回寄存器和 BRK 等自洽负例，均 FAIL。本次没有找到最终版本的新绕过；这不等于穷尽证明不存在其他绕过。

## 未验证项与边界

- 没有做真实全量链接、最终真实镜像验收、硬件运行或 stock I/O/栈/时序验证；没有重跑与本次修改无关的 recorder 运行测试。
- 本证明止于首次到达 stock 地址，不证明调用参数、stock 返回之后的行为或整个 recorder 语义。
- 指令模型以正常 ABI 栈和内存执行为前提，不证明内存访问无故障、栈容量或外部中断行为；读取结果保守视为未知。
- 条件两边都要求到达 stock，模型对提前 RET 和循环一律拒绝，也可能拒绝正常的 UI 拦截早退或重试路径。真实复杂 wrapper 是否能通过当前保守模型未验收；synthetic PASS 不能替代这一项。没有为获得 PASS 放宽检查，也没有修改运行代码来迎合检查器。
