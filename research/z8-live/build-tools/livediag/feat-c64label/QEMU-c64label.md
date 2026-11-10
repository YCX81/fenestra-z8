# win-lab：state2 C10/C11 验证步骤（本次未执行）

使用实验室现有 `~/.local/share/nikon-z8-qemu/imenu-c64/run_imenu.py`，本目录 `run_imenu.reference.py` 是此次核对的副本。不要直接在 Mac 运行/导入副本：它在模块顶层解析参数并启动流程。本次没有连接 win-lab，也没有启动 QEMU。

## 槽位与卡前置

1. 准备实验副本 FENESTRA 目录，包含与该 MAIN 匹配、能自动激活的 `slotcard.wlm` 和激活记录。驱动把该目录顶层文件复制到虚拟卡 `FENESTRA/`；`--store DIR` 另加入 `DIR/sc*.rec`。文件名 glob 区分大小写，来自卡镜像的大写文件要在实验副本规范为小写。每代 sc 文件可能引用更早一代，不能只复制最新一个头文件；保留完整依赖存档。
2. 照片 bank0 至少有 C10、C11 的有效记录。为了稳定一次抓到 C11→C10→C9，准备照片 C9 也有效且持久选择为 **C11，selector=0x100a** 的存档。可在实验室用原来的导入/选择/存卡流程生成；不得只改 .rec 的 selection 字节而不更新 CRC。若现成存档选择是 C64，且 C10..C64 连续有效，则用 `--lefts 54` 可走 C64→…→C11→C10；稀疏槽位必须按实际行数组计算按键数。
3. 只将 `.NP3` 放到卡根不等于已装入 C10/C11。`--np3 FILE`/`--np3 DIR` 仅复制到卡根，目录模式只匹配 `*.NP3`；`--plan imenu` 不执行导入。没有 sc 存档时先在实验室导入 NP3 到目标扩展槽并保存存档。`--plan np3` 的导航是另一条菜单路线，不要拿其“看见文件”截图充当 state2 验证。
4. MAIN 当前运行上限 `*(u32*)0xa116d3400` 必须 ≥11；C32 足够测 C10/11，测 C64 时上限必须64且该槽有效。slotcard 状态位于 `0xa116d3000`：参考模块检查 magic=0x53434431、state=4、存档恢复 load=0、inject_failed=0、apply_failed=0，并核对实际记录和选择；不要只凭固定等待秒数认定注入成功。

**必须使用 card-scheme keys。** 已核对两份同名 Layout 存在差异：

- 正确版本：`_bundle-20261102/slotcard/tools/z8_slots_layout.py`，扩展槽 `part=0,1,2`：
  `(part != 1 ? 0x01000000 : 0) | ((0x7c+bank)<<8) | ((slot-1)<<2) | part`。
- 旧 `slotcode/tools/z8_slots_layout.py` 使用 `(slot-10)*6+bank*3` 分配，是另一方案。不可用它生成本次卡记录。
- MAIN 稀疏初始化 RAM 中实际查到了 C10 card keys `01007c24 / 00007c25 / 01007c26`；模块 `slots_state.inc` 也按 card-scheme 校验整条记录，旧 keys 会被拒绝。

| 槽位 | bank | selector | 三个 keys | RAM record 起点 |
|---|---:|---|---|---|
| C10 | 0 照片 | 0x1009 | 01007c24 / 00007c25 / 01007c26 | 0xa1166d480 |
| C11 | 0 照片 | 0x100a | 01007c28 / 00007c29 / 01007c2a | 0xa1166d880 |
| C10 | 1 视频 | 0x2009 | 01007d24 / 00007d25 / 01007d26 | 0xa1167d480 |
| C11 | 1 视频 | 0x200a | 01007d28 / 00007d29 / 01007d2a | 0xa1167d880 |

记录长0x400，起点公式 **`0xa1166b080+(bank*64+slot-1)*0x400`**，前16字节为 LE `<IIII> = (1,key0,key1,key2)`。`0x40b14440..5c` 的反汇编直接证实 base 为 `0xa1166b080` 且 flag 必须1。

驱动副本 `slot_state()` 的 `records` 快照用了 **0xa1166b000**，少了0x80，读到0不能证明槽位为空。实验室先在自己的驱动副本中将该表达式改成0xa1166b080，或独立读上表地址；这是 harness 观测修正，不是 MAIN 补丁。另确保导入的 Layout 有 `status` region（card版本有，旧slotcode版本没有），否则只会出现 `status_error`。

## 按键及启动命令

前提：恢复照片选择 C11，打开 i 菜单时焦点在 Picture Control 格。驱动不主动寻找 PC 格；若焦点不同，先在实验室配置好，不能把第一次 OK 进入别的格当作本测试。照片/视频开关先用默认照片。

实际 `imenu` 实现，不是文件开头旧注释的 `up x6`：

```text
半按快门 → 释放
5轮半按/释放保活，等待 slotcard 注入
i → 释放（等4秒）
OK → 释放（等4秒）              # i→PC→OK，state2，选中C11
Left → 释放（--gap）           # 选中C10
Left → 释放（--gap）           # 选中C9，原厂对照
OK → 释放                     # 确认并退出
半按 → 释放 → i → OK → 等待    # 自动重进，检查往返/缓存
```

按住每个导航键0.4秒，每次有释放帧。参数 `--lefts 2` 正好产生两个 Left；不是翻两整页，也不是直接定位 C10。仅显示 C10/C11 可用 `--lefts 1`，增加一次 Left 是为了同时对照 C9。i 的 MCU 三元组为 `(0x48,0,0x400)`，OK 为 `(0x2a,0x8000,0)`，Left 由实验室 probe 的 `navigation_contact('left')` 提供。无需使用 `--keys` 替换正常 OK。

以下命令**交由用户在 win-lab WSL 执行**，路径变量按实验室现有位置填写；每组使用相同的源卡目录/存档，让驱动生成独立虚拟卡：

```sh
LAB=$HOME/.local/share/nikon-z8-qemu
DRIVER=$LAB/imenu-c64/run_imenu.py
FENESTRA=/path/to/compatible-fenestra
STORE=/path/to/card-scheme-store-selected-C11
BASELINE=/path/to/bg34.main
CANDIDATE=/path/to/bg34.c64label28.main

python3 "$DRIVER" --name c64label28-base-photo-01 \
  --main "$BASELINE" --plan imenu --fenestra "$FENESTRA" --store "$STORE" \
  --lefts 2 --gap 3 --seconds 900

# 基线进程结束后运行候选；距上一次 QEMU 启动至少90秒，使用新的 name。
python3 "$DRIVER" --name c64label28-patch-photo-01 \
  --main "$CANDIDATE" --plan imenu --fenestra "$FENESTRA" --store "$STORE" \
  --lefts 2 --gap 3 --seconds 900
```

`--name` 重复会拒绝；实验室驱动会检查近期 QEMU 启动。若 C11 没恢复，不能继续套用两个 Left 的结论：看 `pcsel` 前两项（0xa02983cf8/cfc）和 `menu.rows`/截图中的实际选择。所有有效扩展行按 slot 顺序加入，跳过空记录；C11、C10同时有效时相邻。只有从更高槽位走回来才需要按实际非空槽位个数计算 Left 次数。

独立视频组需 bank1 也具备对应存档/选择，再加 `--video`。它只保持物理 photo/video 选择开关的 MCU 位，不会替你设置“视频跟随照片/独立”的菜单设置；须分别准备和核对跟随/独立状态。

## 截图与通过条件

输出在 `$LAB/imenu-c64/runs/<name>/`，保存 PNG、`imenu-checkpoints.jsonl`、驱动结果和 MAIN 哈希。每步有截图/checkpoint，重点查 `k01-ok`、`k02-left`、`k03-left` 及相邻 release/下一步截图；截图钩子还用于按键前准备，**不要只按文件名推断它已执行该键**，以实际 state2 和 row ID/选中内容认定 C11、C10、C9。

扩展 state2 行识别（不是 title ID，也不是 selector）：

- C10 word0=0x7e00..0x7e04，word +0x18=0xa028。
- C11 word0=0x7e05..0x7e09，word +0x18=0xa029。
- 行 +0x14 是独立示例图；图下 widget 为 kind3。

必须截到整条示例图区域及下方文字，至少包含：C11选中、C10选中、C9选中（旁边的非选中 C10/C11）；以及退出重进。黄色背景里完整的实际编号才算该项通过，不能以 MAIN 中存在字符串、槽位 flag=1 或解析器被调用替代像素证据。

若需要用实验室调试器观察，原/新 resolver 最后 `0x40b12444` 写回样式：在该点检查 x9 描述符、[x9] ASCII指针、[x9+8] length=4、w10，以及随后 [x3]。基线w10=0x21f、候选w10=0x229；当前描述符补丁的 **[x9+12] 也应为0x229**。不要使用旧 `c64label-229.main` 三字节 resolver 实验混作本组候选。原厂 C9应走0x40b1245c回退。

候选变为28号但仍空白：记录实际kind、指针/长度、绘制框和裁切，而不是立即追加PNG。四字符可能临界超宽；详细风险见 C64LABEL.md。当前扩展文本没有星号，不能用该候选做“星号宽度已通过”的结论。

共享标签影响须另测拨盘快速选择条及 OSD、原厂↔扩展往返；NFP/Live 文件字节未变不能代替这些界面回归。只有基线也受 readiness 卡住时，另开新name的 `--unlock` 诊断组；它会在按键前写guest `0xa032e78a0=0`，属于 harness override，必须与自然按键结果分列。到不了 state2、MMIO异常、只有拍摄页截图均记未完成，不宣称显示修复。
