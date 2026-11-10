用中文回答。修改 armed-only 记录器 src-fix35/recorder_patch.py、src-fix35/recorder/ 并同步 recorder-build/。禁止 USB、刷写、操作相机、QEMU。背景见 ANALYZE1-RESULT.md：硬件上武装成功（prepare 写卡在真机正常），Live→切录像→REC 后死机重启，文件只有事件 0，事件 1/2/13 全无，无法定位。USB 连着不能录像，死机窗口 USB 也读不到，所以卡文件是唯一证据。
目标：下一次复现必须能区分“没到 bring-up hook / 被门控拒绝 / 卡在 hand-back 内 / hand-back 返回后才出事 / 记录器自身 I/O 卡住”。
要求：
1. 在共享 bring-up 入口，hand-back 调用**之前**，armed 且精确 LR=40d07ba8 时落卡一条新事件（入口记录，含 LR、client、模式、epoch、shutdown）。不做 client 门控。原 hand-back 函数体逐字不变，返回值、寄存器、NZCV 恢复照旧。
2. 事件 1（hand-back 返回后）取消 client<2 的门控，任意 client 都记录并写入实际值。
3. 门控拒绝不再静默：epoch 不同、shutdown、arm≠generation、ready=0、claimed、模式不符等，只要精确 LR 匹配，就把拒绝原因写一条拒绝记录（整个 boot 最多一条）；epoch 变化本身不再阻止写卡（每事务都重新 OPEN，安全），只标记。若 runtime 无效则不写。
4. 文件扩为足够容纳：准备、入口、hand-back 后、拒绝、service rc、完成，各固定扇区，预分配，同步更新 decoder.py、判定表、arm.py 的总大小，并让 arm.py/状态读取打印当前 runtime epoch 与 shutdown。魔数/版本升级，旧文件不被误解。
5. 未 armed 路径成本不增加（保持现有 111 指令/32 字节门控）。写卡仍是逐事务 OPEN/SEEK/WRITE/CLOSE。入口记录写在 REC 任务上下文，写失败不得阻止 hand-back，不重试。
6. 更新并运行全部测试（门控、IO、并发、verify_link 负例）。verify_link 不要继续扩展 CFG 证明，保持现状。如实报告通过项和未验证项，并说明新增写卡点对现象的可能干扰。
