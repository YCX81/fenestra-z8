用中文回答。在 v3b 基础上实现灯码诊断版，修改 src-fix35/recorder_patch.py、src-fix35/recorder/ 并同步 recorder-build/，新增手机视频解码工具。禁止 USB、刷写、操作相机、QEMU。设计依据 ANALYZE2-RESULT.md 第 2 节方案甲，v3b 真实产物在 v3b/ 供核对地址（注意你修改后地址会变）。
要求：
1. 保留准备事务和事件 15 入口事务；入口之后，REC 路径不再有任何记录器文件 I/O（事件 1、16、2、13 的落卡全部取消，也不做完整 Live 快照）。hand-back、close 算法、超时都不变。
2. 四个灯码点：I（fr_before 返回后、调用 live_w5_bringup_notify 前，恢复 x0–x7/NZCV 之前发码）；R（该 BL 紧接返回后，先于 fr_enabled）；S（0x402f303c，原始 W0、W19）；M（0x402f3068 的 LDP 之前，读 *(u8*)(0xa02e98480+W19)）。入口以完整 LR=0x40d07ba8 锁定一次会话（一个 boot 一次）；S/M 用该会话的原 SP 或 LR 链核实归属，不靠事件 1。写清楚 trampoline 重放的原指令及恢复点。
3. 编码按 ANALYZE2-RESULT：同步头 灭200/亮250/灭150 ms，Manchester 半位 50 ms（亮灭=0，灭亮=1），阶段 2 位+值 3 位+CRC4；S 的 0–5 原样，其余逃逸追加 32 位；M 的 0/1/ff 编码，其余追加 8 位。用计时器定截止时间，等待用现有 DELAY 让出任务，禁止忙等；核实 DELAY 与 GPIO 接口 0x40c15c3c(0xa0280a1b0, level) 可在这四点调用。
4. 未 armed 路径不增加成本（111 指令/32 字节门控，或说明差异）。新增 hook 的保存/恢复按 v3 的 784 字节完整帧，包含 SIMD、NZCV、FPCR/FPSR。
5. decoder 工具：输入手机视频与 ROI 坐标（或自动找灯），输出每帧亮度序列、解出的帧（含 CRC 结果）、时间戳，并有合成视频/序列的单元测试（含 60fps 抖动、原厂约 1 秒长亮干扰、CRC 错误拒绝）。
6. 给出上机前对照流程：未跑 Live 的正常 REC（应四帧齐全，或说明 S/M 正常值）。如需要武装才启用，沿用 arm 机制；文件名/魔数升级避免混淆。
7. 更新并运行所有相关测试，verify_link 不要扩展 CFG；如实报告通过项和未验证项，并给出额外延时的实际估算。
