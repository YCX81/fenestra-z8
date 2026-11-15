# v51d Live / REC 完整回归与 QEMU 配置对照

**同一 v51d 固件在修正后的 QEMU 配置中通过完整 Live 拍摄 → REC 预览 → 半按退出 → Live 恢复回归。** 固件没有为此修改，所有原始断言、相机等待参数和驱动 deadline 保留。此前失败证据全部保留。

唯一功能配置差异是开启当前同一 QEMU binary 已存在的 `z8-heif-secondary-stop-model=on`。实际 `-M` 参数逐项比较，删去这一项后完全一致。On 轮另增加六个被动 PC/寄存器观测点，以及最终断言之后的四次只读 QOM 查询；这项观察差异明确保留在结论边界中。

## 固件与测试身份

| 项目 | SHA-256 |
|---|---|
| v51d MAIN | `aa6e94e0042fb445c666de73c0f6b9e7e569010b7993a3ac3da0979b174a9433` |
| 共同 QEMU binary | `5ccbfc7e051a78dd24176aec98b3ab767709bc078dd3e179923c74b2f50a2ac2` |
| Off driver | `e9c15f2eb268f33805a41b301c17eef58880f7f0ccae021001c86cf183b5de5e` |
| On / 固定验证 driver | `1ec568271fd93739a4d92005b56790e765f19a9da6f3a1782e8a84ccb45a6add` |

## 实际结果

| 项目 | Off：v51d-a2-wait-observer | On：v51d-secondary-stop-model |
|---|---|---|
| Live 写出事务 / 错误 | 1 / 0 | 1 / 0 |
| REC 预览帧数 / 错误 | 37 / 0 | 45 / 0 |
| 半按返回 IRQ | 1588 不增长 | 1582 → 2078 |
| 半按返回编码数 | 413 不增长 | 413 → 659，最终 707 |
| 原始恢复断言 | 失败 | 通过 |
| capture kind / 历史错误 | `0x40` / `A2` | 无 |
| 驱动退出 / 完整 trace | 1 / false | 0 / true |
| Ring 覆盖 / 寄存器读取错误 | 0 / 0 | 0 / 0 |
| 输入文件核对 / 自有 QEMU 停止 | 通过 / 是 | 通过 / 是 |

Off 轮的实测错误链为 `4115fbe4` 失败分支 → `40f5b2a0(status=0x100, LR=40f55b60)` → `40f532bc(LR=40f4b600)` → kind `0x40` → A2。它发生在进入回放期间、REC 按键之前。随后另一个 `41155434` 的 `0x1000` 等待成功，排除了最初的静态候选。详细原始轨迹见 `A2-TRIGGER.md`。

## 实际 mask、通知和返回值

On 轮直接在 `4115fbbc` 采集参数、在 `4115fbc0` 采集返回值，共八次等待，全部返回 1（成功）；超时参数均为原始 `0x96`。其中七次 mask `0x40`，一次 mask `0x40040`。RTOS mode 2 是 AND 条件，因此 `0x40040` 需要 `0x40` 与 `0x40000` 同时满足。

`0x40040` 这一次发生在 step-046-ui-menu 与 step-047-ui-menu-release 之间，先后顺序完整记录：

| ring seq | 观测 |
|---|---|
| 7352 | 进入 `4115faf8` |
| 7353 | `41151a5c` 对 OS event 对象 `0xa0496efe0` 置 `0x40` |
| 7355 | `41151ed8` 对同一对象置 `0x40000` |
| 7356 | `4115fbbc` 实际参数：对象 `0xa0496efe0`、mask `0x40040`、timeout `0x96` |
| 7357 | `4115fbc0` 返回 1 |

同轮 stderr 第 26986 行实际出现：

```text
Z8_1508_SECONDARY_STOP cpu=0 pc=40c2b854 pending=40000 notifications=1; drained status source, no pixels
```

最终 `z8-heif-secondary-stop-state` 为：

```json
{"enabled":true,"armed":false,"notifications":1,"physical_latency_verified":false,"pixel_bytes":0}
```

最终 global1 pending 为 0、IRQ 为 false；channel timing 仍 active。后续 Live 拍摄后的回放切换（seq 23325–23328）实际 mask 是 `0x40`、返回 1，且 REC 结束后 Live IRQ/编码恢复。**不把菜单阶段这一次 secondary 通知误写成发生在拍摄后的回放切换。** Off 轮没有直接采到失败等待的 mask，所以也不倒推出它必定是 `0x40040`。

已有模型仅在实际 group1 latched/active、secondary timing active、未挂起相应通知时 arm，再在停止、source1 clear 且 drained 后产生一次 global1 bit18。没有新增完成事件实现、写 guest flag、补像素、修改 deadline 或绕过原断言。

## 已固定的可重跑配置

On driver 已从临时实验路径复制成独立固定目录，字节 SHA 与实际通过轮一致。该 driver 总是开启上述既有模型，并断言基础 profile 尚未重复设置它。

- 远端 driver：`/path/to/user/fxfix/v50-playback/validated-v51d-recovery-driver/retry-driver/run_live.py`
- 本地完整 driver 与依赖副本：`validated-v51d-recovery-driver/`，另有同名 `.tar.gz`
- 远端 launcher：`/path/to/user/fxfix/v50-playback/launch_validated_recovery.sh`
- 本地 launcher：`launch_validated_recovery.sh`

在当前 win-lab WSL 环境直接重跑：

```sh
bash /path/to/user/fxfix/v50-playback/launch_validated_recovery.sh UNIQUE_RUN_NAME
```

launcher 要求固件、QEMU binary 和 driver SHA 全部匹配，再启动独立输出目录与卡镜像。当前 remote 环境依赖和模型 binary 保持原样；脚本 `bash -n` 通过。本次不额外重跑已通过的同一配置。

## 归档与结论边界

- 紧凑最终验收：`v51d-core-regression-final.json`。
- On 全部实际等待参数、事件、final QOM：`qemu-v51d-secondary-stop.json`。
- Off 实际失败链：`qemu-v51d-a2-wait.json`。
- 两轮完整 `result.json`、原始 plugin ring、stderr、截图、MOV、driver、offsets 和文件 SHA 均在 `recovery-failure-evidence/v51d-a2-wait-observer/` 与 `recovery-failure-evidence/v51d-secondary-stop-model/`，同时保留对应 tar.gz。
- On MOV SHA-256：`15c4a5095b3a854f3bfa2091fd781cb7e67225f0310b5f49b1298a3111a7d72f`。

本次可以标记 **Live 拍摄 / REC 预览 / 退出恢复回归在固定的修正 QEMU profile 中通过**。这是测试配置修正，固件未改。单次对照不证明所有历史失败的唯一原因、不证明真机资源让路或物理时序，也不改变原厂 MOV 全片回放的独立模型边界。v49 菜单 early A2、v50 先前成功与失败、v51d Off 失败记录仍作为原始证据保存。
