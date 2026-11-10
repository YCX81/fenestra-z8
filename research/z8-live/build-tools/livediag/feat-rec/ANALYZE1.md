用中文做只读根因分析。不要修改文件；禁止 USB、刷写、操作相机、QEMU。严格区分【已证明】与【推测】。
硬件结果（镜像 ce1879c8，linked-recorder/ 有真实 ELF/对象，W5F00001-run1.BIN 为实际记录）：
武装成功：USB 读到 arm=1 attempted=1 ready=1 generation=1 seq=1，tx0 phase16 written 2048 close 0，claimed=0。然后拔 USB，拍 Live，切录像模式，马上按 REC，相机死机并重启（与之前相同）。重启后文件仍是 2048 字节，只有事件 0（准备记录），事件 1/2/13 都没有落卡。
此前硬件证据：v34 在 hook 处亮灯约 1.7 s 后熄灭（hook 看起来返回）；切模式时无灯；录制不启动；约 18–27 秒后重启；无 Live 冷启动 REC 正常；USB 连接时由电脑执行关闭后 REC 正常。W5-BRINGUP.md 和 src-fix35/recorder/README.md 有背景。
任务：
1. 对照真实对象反汇编（fr_entry_1/共享 bring-up 路径、fr_probe、record、persist、门控），穷举“事件 1 未落卡”的全部可能路径，例如：bring-up 入口未到、hand-back 自身未返回、LR/client/模式门控拒绝（注意 client 可能在此时不是 0/1）、ready 被清除、写锁争用、OPEN/SEEK/WRITE/CLOSE 同步挂起、写完未持久化、拍 Live 时 worker 或卡状态变化使 ready/card_epoch 失效。对每条给出现有证据能否排除。
2. 特别检查：拍一张 Live 之后、REC 之前，有什么会让 ready 归零或会话失效（card epoch、Live finalize 等）——如果记录器设计使 Live 之后无法记录，这是诊断版自身缺陷，要明确指出。
3. 按概率排序，并给出能一次区分最多假设的最小下一步实验。优先不刷机的做法（例如 USB 读 RAM、观察卡灯）；确实需要改固件时，只说明最小改动，比如把关键进度写到重启后仍保留的位置，并核实这种位置是否存在。
结论要短，只列有证据的内容。
