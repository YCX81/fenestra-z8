# win-lab 离线候选的 QEMU 验证步骤（本次没有执行）

本次只通过 SSH **读取**了实验室工具和目录，没有远程写文件、启动 QEMU 或操作设备。下面命令由用户在 win-lab 的 WSL 执行。`run_c64label_lab.py` 对原驱动做内存中的有校验扩展；原文件保持不变。驱动哈希必须与随附副本一致：`9f50a3d5e7b5ab786c056cf2605d3d70ecfae20fb1b41f6730c6b839ad65d6aa`。

## 1. 输入、槽位与 C9 前置

把 `bg34.main`、`bg34.c64label28.main`、`run_imenu.reference.py`、`run_c64label_lab.py`、`prepare_c64label_records.py` 放在 WSL 的 `~/.local/share/nikon-z8-qemu/imenu-c64/candidate-c64label/`。

必须满足：

- `FENESTRA/slotcard.wlm` 已自动激活；参考目录 `fen-f568` 含 `STARTUP-Z8.0` 和该模块（本次只读确认存在）。启动日志需显示注入成功。
- 卡上不仅要有 NP3：**`--np3` 只把文件放进卡根，不会自动导入 C10/C11**。可靠的自动化前置是先生成真实原厂转换后的 1024 字节槽位记录，再用模块自身的存储代码生成完整 `sc*.rec`。不要改文件名冒充槽位，不要只改有效标记，不要用 `--garbage` 预加载记录；启动会清零保留区。
- 使用 **card-scheme** 的 keys，而不是老 NAND 扩展 scheme。`prepare_c64label_records.py` 用实验室 `NativeSlots`（Unicorn 函数仿真，不是 QEMU）做原厂 NP3 转换，再按 `~/Nikon/_bundle-20261102/slotcard/tools/z8_slots_layout.py` 重写三个 keys，与已存在的 `mklib.py` 做法相同。
- C9 是原厂槽位，slotcard **不会恢复 C1..C9**。要得到“选中 C9”的对照截图，实验室 NAND/原厂设置 fixture 必须已经有有效 C9；如果启动后左移从 C10 直接跳到原厂预设，说明 C9 不存在，此轮不能声称完成 C9 对照。可在**同一 QEMU 会话**中经 MENU → 照片拍摄菜单 → 管理优化校准 → 载入/保存 → 复制到照相机，把实际 NP3 导入 C9 后再做以下按键序列；仅在另一次新 NAND 会话导入无效。现有 `run_imenu.py` 没有复用 NAND 的 CLI，本说明不虚构 `--nand` 参数。无人值守运行以下命令前，请使用已有含有效 C9 的实验室基础 fixture。

生成 C10/C11 的两个 bank 存档（全是实验室目录；不访问挂载卡）：

```bash
set -euo pipefail
LAB="$HOME/.local/share/nikon-z8-qemu/imenu-c64"
WORK="$LAB/candidate-c64label"
PY="$HOME/.local/share/nikon-z8-qemu/venv/bin/python"
STAMP="$(date +%Y%m%d-%H%M%S)"
CASE="$WORK/fixture-$STAMP"
mkdir -p "$CASE/store"
"$PY" -B "$WORK/prepare_c64label_records.py" --out "$CASE/records"
# 可加 --np3 "$LAB/np3root/某个真实文件.NP3"，否则生成 FC 测试 NP3。
SEL=1009,2009 "$LAB/mkstore" "$CASE/store" "$CASE"/records/*.bin
# 应有 commit/reload 成功、used 4；保留全部 sc*.rec，不可只复制最后一代。
find "$CASE/store" -maxdepth 1 -name 'sc*.rec' -ls
```

映射（原厂转换结果的 +0..15 为 `valid,key0,key1,key2`）：

| bank/slot | 存储 index / 文件 | selector | card keys |
|---|---|---|---|
| photo C10 | 9 / 009.bin | 0x1009 | 0x01007c00, 0x00007c01, 0x01007c02 |
| photo C11 | 10 / 010.bin | 0x100a | 0x01007c06, 0x00007c07, 0x01007c08 |
| video C10 | 73 / 073.bin | 0x2009 | 0x01007c03, 0x00007c04, 0x01007c05 |
| video C11 | 74 / 074.bin | 0x200a | 0x01007c09, 0x00007c0a, 0x01007c0b |

RAM 记录公式为 `0xa1166b080 + index*0x400`，第一 word 必须为1。注意原 `run_imenu.reference.py` 的 `slot_state.records` 从 `0xa1166b000` 起读，少了0x80；随附 wrapper 仅修正这项诊断，不能把原日志里的0当成加载失败。

## 2. 截图序列与命令

从恢复选择 **C10**、i 菜单当前格为 Picture Control 开始，序列为：

1. 等待 slotcard 注入（原驱动的5轮半按唤醒/等待保留）。
2. `i → OK`：state2，当前 C10。
3. `left`：C9；`right`：C10；`right`：C11；`left`：回 C10。每个按键均释放并等待4秒、由原驱动截屏。
4. `OK` 确定退出；`i` 重开 i 菜单；主拨盘 `+1` 再 `-1`：快速选择条返回 C10。

这里的拨盘是真实 MCU 输入 `body[0x0b]=±1`、decoded word4=±1，依据实验室 `z8_hsfc_controls.py`，不是把 left 键截图当拨盘截图。拨盘方向的 UI 含义、是否触发选择条仍以实际截图为准；两步相反增量用来回到起点。若镜像的按键焦点/模式与前置不同，用 `--actions` 调整；不要仅凭命名断言已经到达目标页面。

先只校验/编译（不运行 QEMU）：

```bash
"$PY" -B "$WORK/run_c64label_lab.py" --driver "$LAB/run_imenu.py" --prepare-only
```

基线与候选都跑相同输入，顺序运行，起动间隔至少90秒（使用100秒）：

```bash
for WHICH in baseline patched; do
  if [ "$WHICH" = baseline ]; then MAIN="$WORK/bg34.main"; else MAIN="$WORK/bg34.c64label28.main"; fi
  NAME="c64label-$WHICH-$STAMP"
  "$PY" -u -B "$WORK/run_c64label_lab.py" \
    --driver "$LAB/run_imenu.py" \
    --actions 'i,ok,left,right,right,left,ok,i,dial+,dial-' \
    --name "$NAME" --main "$MAIN" --plan imenu \
    --fenestra "$LAB/fen-f568" --store "$CASE/store" --seconds 1200 \
    > "$CASE/$NAME.log" 2>&1
  sleep 100
 done
```

独立视频重复同一命令，加 `--video` 并更换 `--name`。不自动启用 `--unlock`：若 lock 的 readiness 位持续不清导致键无效，可另开一轮加 `--unlock`，单独记录为 harness override，不与无 override 的结果混称。

原 `--plan imenu --lefts 6` 的 left 数量是固定步数，**不保证落在 C10**；随附 wrapper 改为以已恢复 C10 为锚点。当前实现尚未经过 QEMU 验证，只完成 hash 校验、替换点校验和编译。

## 3. 验收与失败判定

输出目录：`$LAB/runs/$NAME/`；原始 PNG 与 `imenu-checkpoints.jsonl` 一并保存。驱动是在联系人序列的 checkpoint 截图，应用截图应取按键之后（通常对应 release/下一 checkpoint），不可只按文件名推断。

```bash
python3 - "$LAB/runs/c64label-patched-$STAMP/imenu-checkpoints.jsonl" <<'PY'
import json,sys
for line in open(sys.argv[1]):
    x=json.loads(line)
    print(x['label'], 'imenu=',x.get('imenu'), 'menu=',x.get('menu'),
          'pcsel=',x.get('pcsel'), 'slots=',x.get('slots'),
          'style=',hex(x.get('c64_style_7e00',0)))
PY
```

- state2 以诊断 `imenu.state == 2`、实际 C9/C10/C11 标签/选中背景共同确认；对照相邻槽位、选择器和画面，不能只凭 row index 判断稀疏槽位编号。
- 候选 `c64_style_7e00 == 0x229`，基线为0x21f。只有到达初始化后的截图才有效。
- 拨盘截图要显示快速选择条且当前为 C10；确认调用链（可额外用 `--points kind:addr,...`），或至少保存拨动前后原图和选择值。
- C9 字体/布局应与基线相同；C10/C11 应可见，黄色选中与未选中都看，检查右边缘、基线、高度及字符完整性。
- 任一槽位未载入、C9 缺失、状态错误、键未被消费或进入超时，判为**前置/驱动未完成**，不能判“字体补丁无效”。
- 还应检查主菜单 PC 当前值/列表、Copy to camera 目标列表、照片 LiveView PC OSD；它们也是本次明确接受的共享字号变化。
