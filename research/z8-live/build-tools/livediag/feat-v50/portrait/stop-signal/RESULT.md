# A2 等待 producer：只读追踪结果

分析镜像为 v51d MAIN `aa6e94e0042fb445c666de73c0f6b9e7e569010b7993a3ac3da0979b174a9433`。运行证据由 playback Agent 的 `v51d-a2-wait-observer` 归档提供；本子任务只读固件、模型源码及日志，未改固件、模型、完成事件或超时，未接入运行中的 GDB/QMP。

## 已排除的初始猜测

`41155310` 在串行命令0x21后等待OS事件0x1000；对应aux0 IRQ为0x102，ISR `40c29af4` 读并W1C `15085108`，向回调 `41152044` 传事件 `41020402` 和原始pending。其bit0在 `411520c0` 对 `a0496efe0` 置位0x1000。aux1 ISR `40c29b94` 对应IRQ0x104、`15085508`，事件41020404；实际第二组初始化回调为41152c0c，不能仅因为41152044的分派表也接受404就认定当前aux1也写0x1000。

命令0x21不是直接清1508控制：`41166d6c` 的表项到 `41167338`，取运行期配置base `[a0496f6a0]` 的 `+ec990` 字节和 `+ec9c0` 长度，经 `41167b7c → 41177e0c → 40ca4a1c` 串行发送。其前置计数器同步走 `0e001000`，回调4115fc24只置OS事件1。这些计数器不能误认成1508源周期timer。

本轮稀疏probe已观测真正的A2发生在此前的 `4115fbe4`，而随后 `41155434` 的0x1000等待返回1、停止函数返回0。因此“本轮A2由aux0x1000等待失败造成”已经被本轮观测排除。周期aux模型可能有其他边界限制，并不能将其当成本轮根因。

## 实际失败路径与 AND 语义

`4115faf8` 的顺序：

1. `4115fb48` 清OS事件0x40040；`4115fb54` 清0x1000。
2. `4115fb78` 发group-stop命令3049或304a，由字节 `a0496f4e8` 是否非零选择。
3. `4115fb84`、`4115fb94` 调 `41169330(0)` / `(1)`，经 `40cbcc44 → 40c29c88 → 40c2b834` 清两组source。原厂写source0 clear `15080014`，source1 clear `1508001c`。
4. `4115fbbc` 等待 `a0496efe0`，mask为0x40，或该字节非零时0x40040，超时参数0x96。失败在 `4115fbe4` 合入0x100、日志30b6。

本轮观察已实证失败分支、分类输入status=0x100与A2；尚未直接记录本次mask。不能把两个静态可能值之一写成运行事实。

`40d2be54` 传RTOS mode=2。真实谓词 `4017b1ec` 在 `4017b1f4` 检查mode bit0；mode2的bit0为0，走 `4017b1f8..1fc` 比较 `(pending & requested) == requested`。所以这是 **AND等待**；若mask=0x40040，只有primary 0x40到达仍不能成功。不是基于通用RTOS名称猜测语义。底层非零返回均被包装为失败，目前未用本轮原始返回码区分超时与其他wait错误。

## 两个硬件状态到OS位

| 路径 | 原厂ISR与硬件状态 | 原厂OS置位 |
|---|---|---|
| primary | IRQ0xff，`40c2b314` 读/W1C `15080054`，mask来自15080050；callback41151820收到原始pending | pending bit18经4115193c/41151a30，到41151a5c置OS0x40 |
| secondary | IRQ0x101，同一ISR在40c2b3a8读/W1C1508006c，mask来自15080068；callback41151cf8收到原始pending | bit18经41151eac，到41151ed8置OS0x40000 |

此处两组硬件pending都用bit18，但写到不同OS位，不能混为一个完成信号。

## 已有模型行为与本轮未采集项

构建源码由 `build/config-host.mak` 确认是 `/path/to/user/.local/share/nikon-z8-qemu/merged/work/qemu-10.2.4`。二进制SHA `5ccbfc7e051a78dd24176aec98b3ab767709bc078dd3e179923c74b2f50a2ac2`。相关带行号摘录和hash在 `model-evidence.txt`。

`nikon-z9-lab.c:18114` 的既有primary stop模型只有在source0原bit0为1、该bit被清、且primary timing原本active时，才在18133置global0 pending bit18并更新IRQ。本轮实际命令开了 `event1508-stop-model`，stderr有7次 `Z9_1508_STOP`。最后一组stop在81265～81274行，primary通知明确已产生。不能称primary完成未生成。

既有 `nikon-z8-secondary-stop.h` 的P22注释直接对应4115faf8，但本轮未开其独立选项 `z8-heif-secondary-stop-model`。它不是无条件补事件：

- group-stop时要求latched group1含bit1、source1 bit0有效、channel bitmap含bit1、secondary timing当时active、没有同位pending，才armed。
- 随后真实source1 clear必须清掉原有bit0，且armed；还要求secondary timing已不active、output group1已不running、没有同位pending，才发global1 bit18。
- group启动或source重新启用会取消armed。模型不写guest RAM、OS事件、PC或像素；物理完成时序仍未验证。

本轮命令已启用secondary timing，未启用secondary stop。最后source1控制确从1清到0，channel bitmap704b含bit1，日志没有secondary-stop通知。但GROUP日志在命令self-clear之后打印bitmap，显示0不能证明原latched内容。日志亦不含group-stop瞬间secondary timing active和global1 pending bit18，因此**尚不能从本轮日志证明所有armed条件均满足**。

`observed-model-conditions.json` 保存模型off轮的这些观测及缺失项。它们已用下述模型on对照补充；不能倒写成off轮当时已经采得的值。

## 已完成的单变量功能对照

主Agent授权的模型on轮只新增既有 `z8-heif-secondary-stop-model=on` 功能开关，另加6个只读probe和原断言结束后的一次QOM读取。固件、超时、输入和恢复断言不变；变更脚本及hash在 `../../playback/secondary-stop-provenance.json`。执行MAIN仍为aa6e94e0，QEMU二进制仍为5ccbfc7e。

playback Agent 的稀疏probe记录：seq7353 primary置OS0x40，seq7355 secondary在41151ed8置OS0x40000，seq7356实际wait请求mask0x40040/timeout0x96，seq7357返回1。这同时确认本轮AND两位要求和原厂消费者已接受secondary通知。final QOM显示该模型notifications=1、armed=false；说明现有严格事务gate实际命中过，通知随后被原厂消费，而不是无条件设置OS完成事件。

本子任务已独立读取完成后的原始result.json，SHA-256 `9a0da22b783ec22ce967d555aca20e34d1303d81760c7dccfefbdb11cf0177ea`，保存精简投影 `secondary-stop-comparison.json`：退出码0，无失败断言；照片/短片事务1、错误0，REC预览45帧；回到拍摄后capture IRQ从1582增长到2078，Live encoded从413增长到659，最终707。最终原厂错误history全零。**本次指定配置下的完整Live拍摄、REC预览和恢复回归通过**。

因此本次对照已闭合需要的secondary完成路径，原先的A2/恢复失败在该模型配置下消失。该结论只针对这次相同固件的受控功能对照：不将所有历史A2归为唯一原因，也不证明模型所用空管线/物理完成时序等同于真机。没有据此修改固件的错误处理、deadline或真机完成事件。

反汇编文件：`actual-source-disasm.txt`、`primary-completion-disasm.txt`、`global-callback-disasm.txt`、`rtos-wait-disasm.txt`。早期aux/命令21的证据保留在其余 `*-disasm.txt`，用于说明已排除的分支。
