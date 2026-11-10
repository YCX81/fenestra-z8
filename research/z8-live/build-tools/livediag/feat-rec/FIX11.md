用中文回答。按 REVIEW10-RESULT.md 的两项 P2 修改竞争者宽限，只改这一处逻辑，别的不动，同步 recorder-build/。禁止 USB、刷写、操作相机、QEMU。
做法（替代现有动态宽限）：删除 fr_close_grace 的逐槽时长快照与 credit 累计。竞争者进入 close_lock 等待时一次性计算硬截止 deadline = start + 4 s + (本诊断会话 armed 且已领取 ? D 码最大总时长 + 100 ms : 0)，D 最大总时长取编译期常量（六帧最坏情况，含逃逸），之后循环条件只看 now<deadline，超过即按原失败路径返回，无任何“锁空闲再续等”的分支。未 armed 时 deadline 等于原 4 s，循环与原逻辑等价（说明具体指令差异）。
补对象反例：0.2/6.2 s 释放成功；持锁卡死在 deadline 内超时；REVIEW10 的两组交错（时间快照、反复释放/重新取得）都在 deadline 内结束。运行全部测试，如实报告。
