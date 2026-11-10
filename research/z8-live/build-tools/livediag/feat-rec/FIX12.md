用中文回答。按 REVIEW11-RESULT.md 修唯一的 P2，只改 close_lock 等待的截止比较这几行（共享 close 与诊断 close 两处），同步 recorder-build/，别的不动。禁止 USB、刷写、操作相机、QEMU。
做法：恢复原关闭代码的语义——先 CAS 尝试取锁，再用 elapsed=now−start（无符号相减，天然处理回绕）与 budget 比较，budget = 4 s（未 armed）或 4 s + D 最大总时长 + 100 ms（armed 且本会话已领取，进入时一次算好）；时钟频率为零或读数异常时按原代码行为立即走失败返回。未 armed 时与原对象逐指令等价或说明差异。
补对象反例：REVIEW11 的两组（频率读零、跨回绕）都按原对象时间返回；0.2/6.2 s 释放成功；持锁卡死在 budget 内超时。运行全部测试，并输出这两处改动前后的源码 diff。
