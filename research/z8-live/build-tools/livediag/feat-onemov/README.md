> 当前归档至 **V62**：Windows/WSL 构建来源、菜单文字与快门声修复、V61 RAW 配对以及用户实机确认见 [V62 检查点](RELEASE-v62.md)。下文保留 V54–V60 的历史演进。

# one-MOV Live（v54）：只写一个 MOV，机内用原厂播放器播放

2026-11-15。基于 codex 的 v53 G（MAIN c1489408，源码 `feat-v53/integration-g-final/work`）。
产物：`delivery/Z8-v54-Live-OneMov-d417c5a2`（MAIN d417c5a2，Z_8_0406.bin 893f63e2）。仅 QEMU 验证，未上真机。

## 结论

- writer 去掉 MOV 尾部的 LVJ1 预览原子（`live_transaction.c` 里 `live_preview_write` 那一段）。MOV = ftyp + mdat + moov。
- REC 不再走自带 JPEG 播放器，改为进入原厂视频播放页 0x56c 播放 `LIVE\目录\同名.MOV`，结束回到照片页 0x552。
- MOV 留在 LIVE 目录，不进 DCIM、不做 DCF 登记。

## 原厂播放器是怎么被"骗"过去的

原厂播放按媒体 ID 工作（低 4 位是格式，5 = MOV），不是按路径。关键事实：

1. `4085a8e0(id)` → `40865210` → `4085fa38`：先查小列表 `408714e8`，查不到再走主索引：
   `dir = 4085e91c(slot, 目录号)`，`node = 4086c378(dir, id, 0)`。成员格式掩码是 `node+0x24` 的 u16（bit5 = MOV）。
   slot = `*(0xa04822af8 + 8*card)`，C3.11 上是 `0xe149e6dc0`（不是 0xa1…）。
2. 请求的格式与节点主格式不同时，`4085fcc0` 会 `bl 402dadf4` 按路径 stat，失败则整个查找失败。
3. 文件经文件服务打开：`402d9ba0: bl 402dd4f8`（x0 = 路径）。
4. `40711308` 用 ref 的 DCF 记录（`406bc37c(ref)`，调用点 `40711334`）填播放界面信息 `0xa045f08f8`：
   记录 +4 = 帧率代码（3 → (4,2) = 29.97p，非法值 → (10,10)，解码器不会创建）、+5 bit2、+0xb 低 2 位、+0xc/+0xe = 宽高。
   同号 JPG 组里这条记录描述的是照片，所以必须换成短片的值。裁切/比例/资源方案三个补丁都读 `0xa045f08f8+0xc`。
5. 同名 MOV 直接放进 DCIM 时，开机扫描会把它登记成照片的 MOV 成员，`id|5` 原生可用（实验 E2），但记录仍是照片的。

桥接（`pb-fix37/runtime.c`，UI 任务里）：
`nb_start`：DCF 锁 + slot 锁下给节点置 bit5 → `4910 = id|5` → stage=2 → `406364d0()`、`403fb660(1)`、`403ad6a4(3,0x56c)`、`403fb660(0)`。
stage==2 期间四个调用点生效：`pb_open_wrap`(402d9ba0)、`pb_stat_wrap`(4085fcc0) 把 `DCIM\…\stem.MOV` 换成 `LIVE\…\stem.MOV`；
`pb_info_record`(40711334) 返回改好的记录副本；`pb_native_return`(4063b958) 在回 0x552 前清 bit5、恢复 4910。
worker 兜底：不在回放视图或 4910 不再是 `id|5` 持续 0.4 s 就还原。
短片尺寸在 worker 的 `lookup` 里走原子读出（要求 moov 里有 ≥4096 B 的 udta，即带 NCDT 的新格式）。

## 实验记录（win-lab `~/fxfix/onemov`）

| 名称 | 内容 | 结果 |
|---|---|---|
| E1 | `id|5` + 只重定向 open | 查找失败，播放页立即退出 |
| E2 | 同名 MOV 放 DCIM | 原生找到，解码器未创建（记录是照片的） |
| E3–E5 | 置 bit5 | 节点找不到：先用错了查找函数，再是 slot 地址范围判断错 |
| E6 | 置 bit5 + open/stat 重定向 + 宽高 | 进入播放页，音频任务启动，解码器未创建（帧率代码非法） |
| E7 | 换整条记录副本 | 97 次解码，11 次 LPCM，返回照片 |
| om1 | 正式补丁 + writer | 回放同 E7；拍摄写出 1.09 MB MOV |
| om2 | 用 om1 自己拍的文件回放 | 92 次解码，237 项检查通过 |
| om4k | 普通 4K 录像 | 95 次解码，235 项通过 |
| omx / omc | 播放中途按回放键 | 桥接与普通 4K 都停在播放页（模拟器局限），兜底还原未能验证 |

## 重跑

```
cd ~/fxfix/onemov
NB_PEER=1 QEMU=/path/to/user/fxfix/v53-tx16-counter-rdw-v3/qemu-system-aarch64 ./go.sh NAME patch_om1.py CARD.raw [contacts.json]
```
`go.sh` = 打补丁 + `build_onemov.py` + codex 的 `compose53.py` + rdw QEMU + 观察插件 `nbobs.so`（`sum2.py` 解读轨迹）。
Live On 的卡必须用带 codec peer 的 QEMU（`NB_PEER=1`），否则 Live worker 卡在 SPI 读，解码器拿不到。
拍摄回归：`driver/retry-driver/run_live.py --mode on`（codex 驱动的副本，改了期望 SHA），QEMU 用 `v53-tx16-counter-merged-v3`。
打包：`pack54.py MAIN verification54.json README54.txt pack`。

## 未做 / 未验证

- 真机全部未验证；中途退出的还原、播放页内删除被拒（返回 0x0432008b）没有在 QEMU 里走到。
- v51 之前的 Live MOV 不再能机内播放（旧 JPEG 播放器已删，PB 区空出约 1 KB）。
- 帧率固定报 29.97p；Live runtime 里为预览保留的缓冲没有回收。

## v55（2026-11-15，真机 v54 反馈后）

真机 v54：REC 进原厂播放页能播；用户报告快门声 On 拍的片子播到结尾卡死、快门瞬间声音断一下、始终没有快门声。
卡上证据（`hw-20261115/`，`movinfo.py`）：9064/9065/9067 无音轨；9066/9068/9069 有音轨，按下时刻前 50 ms 起有 0.150 s 全零（`live_audio.c` 的 duck）。
LIVELOG 只留最近一次开机，这次没有覆盖到拍摄。

v55 = MAIN 34cf691b，包 `delivery/Z8-v55-Live-OneMov-34cf691b`（bin f00feb78），补丁 `patch_om3.py`：
- 录音失败时写等长静音音轨（`live_audio_silence_get`）；duck 改为单位增益；日志记录 31 = 快门让麦流程计数
  （a = phase | captured<<8 | played<<16 | completed<<24；b = cancelled | errors<<8 | mute<<16 | 原厂静音<<24 | 驱动字<<32 | 麦克风阶段<<40）；删掉无人调用的 `live_preview_write`（Live 区空出 776 B）。
- 试过把信息记录 +0xb 低 2 位随音轨有无置 0/1：置 0 时播放器只出首帧就不动，所以它不是"有无声音"，保持 1。
- 无音轨片段在 QEMU 里用 v54 能播完，卡死未复现；静音音轨是按证据做的修正。
- `cap.sh NAME` = 拍摄回归；`patch_om3s.py` = 强制走静音路径的测试构建。

## v56（2026-11-15，真机 v55 反馈后）

真机 v55：Off 时声音连贯、回放正常；On 时"有些卡死有些不卡死"。卡上（`hw-20261115b/`）：9070–9074 是静音兜底音轨（录音失败，那次开机没有日志）；
9075–9077 录音正常，日志记录 31 显示两张 On 的照片让麦流程 captured=0。
v56 = MAIN c7e0bc51，包 `delivery/Z8-v56-Live-OneMov-c7e0bc51`（bin 39b4c4c1），`patch_om4.py`：`live_shutter_z8.c` 的 `allowed()` 恒返回 0，
让麦流程不再介入。快门区只剩 16 B，改动只能缩不能加。回放层与 v55 字节相同。"卡死由让麦流程引起"是推断。

## v57（2026-11-15）

用户指出 v55 卡死的是 9076（录音正常、让麦未介入）。真机片段 9076/9077 在 QEMU 都能播完，但结尾顺序不同：
9076 先 `403f6e88(0,2)` + `403f6f10` 再切 0x552；9077 直接切 0x552。
v57 = MAIN 5328dc05，包 `delivery/Z8-v57-Live-OneMov-5328dc05`（bin 50775f7c），`patch_om5.py`：
返回钩子不再在 UI 任务里拿 DCF 锁，只置 stage=3；stage>=2 时重定向/信息记录仍有效；worker 1.5 s 后在 DCF 锁内清 bit5。
`nb_start` 在同一把锁内置位并置 stage=2。验证：9076 连播两次（观察序列 2→4→3 两轮）、9077 一次。Live 层与 v56 字节相同。

## v58（2026-11-15）

用户：9072、9076 在 v56 上每次都卡死（两张都是 100 帧 / 160160 帧音轨）。QEMU 里这类片段结尾是"停止+拆除 → 4910 改回照片 → 解码照片 → 切 0x552"。
推断：worker 的兜底规则（4910 不再是短片 0.4 s 就还原并清 `ui_leaving`）在真机上会在播放页拆除途中触发（照片解码远超 0.4 s），随后往界面挂 Live 标记。
v58 = MAIN d6fe25c2，包 `delivery/Z8-v58-Live-OneMov-d6fe25c2`（bin 746f37ad），`patch_om6.py`：返回钩子不清 `ui_leaving`；stage 3 满 2.5 s 才还原并放行标记；
stage 2 兜底改为离开回放 1 s / 仍在回放但 4910 变了 5 s。验证：9076、9077 各连播两次。卡死仍未在 QEMU 复现。

## v59（2026-11-15）：卡死与帧数有关

对照（`hw-20261115c/`，放进 DCIM 用原厂 OK 键播）：LIV_0010（=9076）、0011（音轨 −1 采样，`trim_audio.py`）、0012（音轨 −1602）都卡死，0013（=9077）正常。
→ 问题在片子本身，与桥接、音轨长度无关。v56–v58 针对桥接的推断都不是原因（改动保留）。
卡死片段帧数 91、91、100、100，均为 3k+1；QEMU 里 3k+1（91、94、100）结尾走 `403f6e88(0,2)`，3k（93、96、99）正常；3k+2 真机正常。根因未知。
v59 = MAIN 16a4ef21，包 `delivery/Z8-v59-Live-OneMov-16a4ef21`（bin 5bd06e99），`patch_om7.py`：`count%3==1` 时丢最后一帧，录音裁到 `count*8008/5`。
验证片（`drop_last_frame.py`，会改 NCTG 0x13 帧数）：0014 = 9076 −1 帧（99，应正常）、0015 = 9077 −1（98，应正常）、0016 = 9077 −2（97，应卡死）。待真机结果。

## v60（2026-11-15）：帧数规则正式版，取代 v59

真机确认：LIV_0014（99）正常、0015（98）正常、0016（97）卡死 → 3k+1 帧必卡，规律成立。
v59 的缺陷：只在写卡前 `count--`，没改最后一帧时长，`live_mov.c` 的 `is_cfr()` 判定失败，视频轨退回逐帧时间戳。强制触发才发现。
v60 = MAIN 641725ce，包 `delivery/Z8-v60-Live-OneMov-641725ce`（bin 29d80ac7）：规则放进 `capture/live_stream.c` 的 `freeze()`，
丢最后一个 AU 并把 `clip_end` 设为它的时间戳（CFR 保持，录音窗口同步结束）。
测试构建：`OM_FORCE=1`（定格时强制成 3k+1）、`OM_NORULE=1`（去掉规则）配合 `patch_om7.py`；
无规则 88 帧 → `stop2` 结尾；有规则 90 帧 → 正常结尾；正式构建拍摄通过。
