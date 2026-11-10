# REVIEW10：竞争关闭固定截止

仅替换竞争者宽限，按 `REVIEW10-RESULT.md` 两项 P2 修订。禁止项均未执行：USB、刷写、相机操作、QEMU。对象执行使用 Unicorn；原生服务、时间、发码耗时和调度由测试输入提供，不是相机观测。

## 实现及范围

`fr_close_grace(void)` 仅查询当前 armed 会话与 claimed，返回编译期 `FR_D_TOTAL_MAX_US + FR_CLOSE_MARGIN_US` 或零。`fr_context()` 已检查 arm。删除逐槽时长读取、owner 过滤、动态求和与 credit 历史最大值。共享 close 和私有 close 都在入口一次性计算：

```
deadline = start + 4000000 + (armed && claimed ? 6 * 1250000 + 100000 : 0)
```

随后只有 `now_us() < deadline` 才尝试取得锁；每次竞争失败按原 `DELAY(1)` 返回检查。截止后原错误路径设置 close_failed、递增 eos_miss、置 fin_rc 高字节为 11 并返回。没有根据锁空闲续等的分支，没有重新查询宽限。等待期间 arm、owner、D 槽变化不会改写 deadline。

六帧总预算为 7.5 秒，含余量为 7.6 秒，最终 armed/claimed 等待预算为 11.6 秒。当前 D 格式所有 128 个载荷（包含所有 NR 值）均固定 14 位，没有逃逸扩展；host C 遍历全部值验证这一点。同步头 12 个半位，载荷 28 个半位，25ms 半位，名义单帧 1 秒，沿用现有 1.25 秒单帧阈值。I/R/S/M 的逃逸不属于持锁的六帧 D，未改它们的协议。

不改 D 发码、单帧停止阈值、时间记录写入、归属校验、NR、VDI、其他超时或 ABI。旧槽记录保留，但不参与等待决定。源码范围测试严格撤销本次明确改动后比较原始 close；19 个完整链接对象中只有 `codec_live_w5enc.c.o`、`recorder_recorder.c.o` 改变，其他 17 个与 REVIEW9-final 逐字节一致。

## 未 armed 的具体差异

预算仍是原 4 秒，CAS 内存序、竞争后的 DELAY、错误状态写入相同，但不能宣称完全等价：用户指定的严格 `<` 和旧 `elapsed > 4000000` 存在等号边界差异。1ms 时间输入下旧版 4.001 秒失败，新版 4.000 秒失败；新版在每次尝试 CAS 之前检查截止，恰好截止时锁释放也不再成功。原版先 CAS，失败后才查时钟，因此截止后的锁空闲也可能直接成功。按本次指定的硬截止收紧该边界。

本次实际 AArch64 对象（完整反汇编在链接目录 `review10-close-disassembly.txt`）：

- 共享 `w5_close` 在 `0x401eadf4` 新增一次 `BL fr_close_grace`。helper 经 `BL fr_context` 检查 arm，未 armed 时 `CBZ` 返回零；claimed 路径使用一次 `LDAR` 与常量选择。没有循环、槽读取、计数器读取。
- `0x401eadf8` 的 ADD 加入固定宽限，`0x401eae04` / `0x401eae10` 的两条 ADD 加入 4,000,000，deadline 保存在 x20。
- 循环 `0x401eae14` 起读计数器；`0x401eae34` 的 `CMP x8,x20`、`0x401eae38` 的 `B.HS` 直接走原失败写入，之后才有 `LDAXR/STLXR`。DELAY 返回的分支只回到计时比较。
- 同编译参数编译的原始 close 用 `SUB now,start`，比较常数 4,000,001 后 `B.HS`，而且第一次 CAS 在计时检查前。新版去掉每圈 elapsed 减法，以绝对截止比较替代；增加入口 helper 调用及首次 CAS 前的时间采样。共享 close 栈帧仍为 240 字节。不能把编译器寄存器分配和函数布局变化称为逐指令不变。
- 三处未 armed 汇编门控仍各 111 指令／32 字节；这只是门控指标，不掩盖共享 close 新增的调用成本。

## 对象反例

两条真实对象路径 `w5_close` / `live_encoder_preempt_lamp` 均验证，下面时间为自竞争者 start 起的受控时间：

| 场景 | 结果 |
|---|---|
| armed/claimed，0.2 秒释放 | 0.200 秒成功 |
| armed/claimed，6.2 秒释放 | 6.200 秒成功 |
| 六帧后持续持锁／首帧卡死／没有 D | 均 11.600 秒失败，fin_rc 高字节 11 |
| 未 armed 或未 claimed | 4.000 秒失败 |
| REVIEW10 时间快照交错 | 11.600 秒失败；宽限调用一次，owner/时间槽读取零次 |
| REVIEW10 反复释放／重新取得 | 11.600 秒失败；11,600 次 CAS，最后在 11.599 秒，不再越限续等 |
| 等待中 disarm | 仍 11.600 秒失败 |
| 等待中从未 armed 变 armed | 仍 4.000 秒失败 |
| 恰好 11.600 秒释放 | 失败，不执行截止后的 CAS |
| 11.599 秒释放 | 成功 |

时间快照用例使用独立 Unicorn 上的真实 `fr_checkpoint` 写入端：先完成三个 D，再启动竞争者；读者计数器已采样后，后三个 D 分别从更晚时刻开始、各耗时 1 秒，完成真实槽写入后恢复读者。新实现没有旧反例所需的 elapsed/start 读取点，内存读钩子明确断言零次，不能再产生无符号下溢信用。实际总 D 时长仍为 6 秒，固定等待预算始终为 11.6 秒。

反复释放用例在每次计时读取时把锁置空闲，在 CAS 前重新占有；DELAY 内也保持竞争者重新取得锁。即使截止时观察到空闲，循环仍直接返回失败。

墙钟返回上界仍以竞争者获得调度、原生 DELAY 返回为前提；测试不宣称软件能强制抢回失去调度的任务。固定的是截止值和下一次循环检查的决策。

## 产物与复跑

产物：`recorder-build/linked-lamp-v6-review10-final/`。
MAIN SHA256：`8d6e2cc5d2127d0d7332a9a8fe03885cae3a545b4a6b436f9d1fd7704a11d0d9`。

```
.recorder-venv/bin/python src-fix35/recorder/run_checks.py recorder-build recorder-build/linked-lamp-v6-review10-final
```

对象反例详表：链接目录 `review9-tests.json`（37 项原回归，更新宽限期望）、`review10-tests.json`（14 项新增交错／截止反例）。

全套 14 项：13 PASS，1 FAIL；统一命令退出码 1。通过 verify_static、test_gate、test_frame、test_io、test_recorder、test_v6、test_verify_link、test_video_decoder、test_forward_stack、verify_lamp_artifact、test_nr、test_review9、test_review10。1680 条门控对象路径、576 条现场保存／重放路径通过；VDI 1.5 秒清零仍在 1.001 秒判错误 9。

唯一失败为完整 verify_link 的六条既有 forwarding CFG 证明不完整；ELF/MAIN、固定目标、白名单和 hook 重定位结构检查通过。verify_link.py 与修改前及既有基准逐字节一致，未扩大或绕过 CFG。详情为 `recorder-build/recorder-check/lamp-v6-tests/results.json` 和链接目录 `link-check.json`；产物仍标记 `flashable=false`。

首次本地链接因 Zig 默认缓存目录无写权限中断；紧接的对象测试因 ELF 未生成而失败。改用 `/tmp/review10-zig-global` 和 `/tmp/review10-zig-local` 后重新完整编译、链接，再运行上述全套检查。没有使用设备或 QEMU。
