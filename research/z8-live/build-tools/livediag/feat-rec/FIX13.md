用中文回答。基于 v6（src-fix35/、recorder-build/，实验室 MAIN a02d3748）做 v7，只改 live_encoder_preempt_lamp 的 busy 等待段及 D 码编码。禁止 USB、刷写、操作相机、QEMU。
真机结果（runs/a4991/report.txt）：A 组解出 I=0 与 D1（NR=0），之后 24 秒无 D2、无 R/S/M，然后重启。D1 到 D2 之间只有：alive 检查、release_req=1、`while(busy && now_us()-start<4s) DELAY(1)`、超时则 close_failed 并返回（返回后 hook 应发 R）。无论 busy 是否清除，4 秒内都应出现 D2 或 R，但都没有——推断 MOVRS 任务在这段之后再没被调度（DELAY 不返回），或其他原因。
v7 要求：
1. D1 的 2 位 NR 字段在阶段 1 改为携带 busy、alive 两位（解码器按阶段区分含义）。
2. busy 等待循环中，每满约 1 秒发一个心跳 D 码（新阶段号 3），值带 busy 当前值和已等待整秒数（0–3 用 2 位饱和即可，或改用逃逸），发码时间不计入 4 秒预算（预算按不含发码的累计等待时间判定，保持原判定语义）。
3. 超时退出时发新阶段 4（值带 busy=1），然后照原逻辑 close_failed 并返回；!alive 直接返回的分支发新阶段 5。
4. 其余 I/D/R/S/M、skip_nr、归属、硬截止逻辑不变；未 armed 成本不变。
5. 更新解码器、README 判定表（列出：心跳持续出现→任务仍在调度、busy 不清；心跳中断→任务停止调度；出现 4→超时后未发出 R；等等），更新测试并运行全部测试，如实报告。
