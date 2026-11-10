# REVIEW2 修复验证（2026-11-09）

限定本地源码、编译对象及离线测试；无 USB、无刷写、无相机操作、无 QEMU。

## 修复

- `recorder.c`：事件14只要携带启动位且有pending就允许进入原子消费。精确事件1且满足关联条件才绑定adapter；合并事件/拒绝关联消费pending、置DONE、清ready/adapter_gen。claimed保持，后一次REC不能重用旧身份。DONE也可表示诊断终止，不能单独等同stock完成。
- `verify_link.py`：6旧continuation、6旧转发符号由固定SHA的bg34分支独立验证；不以待验ELF值决定期望地址。另解码检查全部9个旧转发wrapper的目标，含3个C常量间接调用。返回/转发证据PC和目标保存在BASE_TRANSFERS/FORWARDS。构建适配器也拒绝与固定目标不一致的manifest。
- `recorder_patch.py`：全保存/C调用之前，用x9–x12及32字节栈进行只读查找；每层指针检查范围、对齐、extent及身份。NZCV在已保存的x11里保留。非零arm才进入recorder的全保存路径；bring-up在已启用分支复用一个完整帧，未启用分支保留原hand-back行为。

## 通过项

在workspace根目录执行：

- 通过`recorder_patch.check()`重编4个修改单元及layout对象。
- 全部19个AArch64单元以源码默认宏、`-Wall -Wextra -Werror`重编通过。命令记录：`recorder-build/recorder-check/allunits/commands.json`。仅编译，无真实全量链接。
- `python3 src-fix35/recorder/test_io.py recorder-build`：原有arm隔离、重试、证据保护、身份拒绝、三次REC写入、200组pthread交错、native失败用例；新增“事件3合并回调→下一次REC”在模式改变、写锁争用和ready=0条件下通过。
- `.recorder-venv/bin/python src-fix35/recorder/test_recorder.py recorder-build`：decoder/事务字段/校验损坏/截断及原有门控结构检查通过。
- `.recorder-venv/bin/python src-fix35/recorder/verify_static.py recorder-build`：基线字节/目标、原hand-back源码不变、对象内联原子检查通过。
- `.recorder-venv/bin/python src-fix35/recorder/test_gate.py recorder-build`：3360个编译对象解释路径通过，覆盖6入口×35条件×16种NZCV。检查寄存器、NZCV、SP和调用者有效栈恢复；写入仅限32字节保存区，不执行C/SIMD。bring-up单帧路径做对象结构检查。完整有效链arm=0是111条指令、32字节；无效指针/身份拒绝14–107条、32字节。结果：`recorder-build/recorder-check/gate-results.json`。
- `.recorder-venv/bin/python src-fix35/recorder/test_verify_link.py recorder-build`：正确synthetic fixture通过，22个负例全部拒绝。含12个旧符号+错误分支同时保持ELF/MAIN一致，以及3个无符号C转发目标破坏。另在真实runtime编译对象中识别并检查3个C常量转发目标。

计数从trampoline入口到恢复点之前，含门控内部最后分支，不含patch-site B、被覆盖stock指令及stock continuation。32字节保存区释放后保留保存过的数据，不宣称抹回SP以下旧字节。共享bring-up仍需原有256字节整数帧及hand-back C深层栈；不能把新增门控的32字节说成整个bring-up峰值。

## 未通过或未验证

- Unicorn在本机调用`mem_map`时进程SIGILL（exit 132），连单条NOP都未执行，不能算作执行通过。替代验证是明确指令子集的Python解释器，不是独立ARM模拟器或硬件证明；未知指令即拒绝。
- 真实全量链接、最终真实ELF/MAIN验收、实际owner编译宏布局、硬件执行、任务栈余量、FP权限、DMA/cache路径、native I/O时延及持久化、stock并发时序均未验证。
- synthetic fixture包含stub，只用于检查器正负测试，绝不代表实际payload已链接。测试结束删除synthetic MAIN，避免误当固件。
