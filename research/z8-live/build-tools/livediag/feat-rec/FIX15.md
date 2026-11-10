用中文回答。按 REVIEW14-RESULT.md 修 v8 的两项，只改这两处，同步 recorder-build/。禁止 USB、刷写、操作相机、QEMU。
1. P1：worker 报告只在录像模式（0xa03bd062c 非零，与 worker 的 photo 判定一致）且会话已 claimed、I 已发时进行；照片模式下一律不发、不阻塞 worker。
2. P2：首次 TRIGGER 前后与“输出等待循环 DELAY 前”使用不同进度号（以及任何其他同号的阻塞点，逐一检查并改成唯一），同步 README 阶段表与解码器。
运行全部测试，并输出两处改动前后的源码 diff。
