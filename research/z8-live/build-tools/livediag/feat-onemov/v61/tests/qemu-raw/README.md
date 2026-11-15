# V61 原生 RAW 锚点 QEMU 回归

这是一张私有虚拟卡上的合成测试。真实 `_DSC0128.NEF` 与模板的 JPG / Live MOV **不是同一次拍摄**；单个 NVMe 卡模型不能验证真实双卡配对。测试没有接触相机、USB 或实体存储卡，也没有更改 MAIN、共享虚拟卡或公共启动器。

最终 MAIN SHA256：`db0c847b333f0a6e07222f2207435e9bf340ce1d544704f6d801ec3ecc1088e5`。

NEF SHA256：`d519a7bb2b7365a9ede204718d8ab80e9d369829d9767d16a40ccdf5649bf3ee`，59,237,510 字节。

最终 `raw-only/summary.json`：**通过**。235 项原生模型检查与 18 项桥接断言全部通过，运行到 40 s 正常结束，未出现 runner error。

| 实际 guest 证据 | 结果 |
| --- | --- |
| selected native / MAN native | 两者均为 `A:\DCIM\100NCZ_8\DSC_0001.NEF` |
| 原 RAW / 虚拟 MOV / 返回 RAW 的 4910 | `0x03200022 → 0x03200025 → 0x03200022` |
| 原生页面 | `0x56c → 0x552` |
| bridge observer | `where2/stage2 → where4/stage3 → where3/stage0` |
| DCF 实际 mask | `0x0024 → 0x0024 → 0x0004`；MOV bit5 撤销、RAW bit2 保留 |
| 原生解码入口 `0x40cfc820` | 2 次 |
| MOV open 重定向 | 2 次，指向 `A:\LIVE\100NCZ_8\DSC_0001.MOV` |

RAW 设置恢复发生在 `where4` observer 之后，随后到 stage0 清位；`selected_events` 及 `trace_index` 保留了顺序。mask 的第一次采样位于置位之后，因此这里证明最终只撤销了临时 MOV 位，并未声称另有一次置位前的内存快照。

可复跑源码在上一级 `run_qemu_raw.py` 与 `raw_nbobs.c`。脚本锁定 MAIN 和 NEF 的 SHA256，只接受不存在的输出目录，先复制模板，然后只修改该私有副本。需要远端现有 QEMU / Nikon lab 运行环境：

```sh
D=/path/to/user/fxfix/onemov/v61-raw-peer-final-d9f5
python3 "$D/raw-anchor-tests/run_qemu_raw.py" \
  --main "$D/composed/q/main.main" \
  --pb-manifest "$D/build/pb/changed-ranges.json" \
  --layout "$D/pb-layout.json" \
  --output "$D/raw-anchor-tests/NEW-PRIVATE-RAW-RUN" \
  --raw-only
```

`--raw-only` 退休私有 DCIM JPG，把 MAN 的 native 路径及大小改成 NEF；Live JPG / MOV 保留。该模式走直接 NEF MAN 查找，只验证原厂 NEF DCF 记录能否作为虚拟 MOV 锚点、完成解码并返回原图。它不覆盖 JPG 成员解析或 other-card 查询；这些由宿主控制流回归分别覆盖，不能替代真机双卡验证。

按键由原有 MCU contact replay 注入：20 s Play，29 s REC，40 s 截止。采用现有 `Z8_TOLERATE=15431000:15432000`，没有增加 MMIO 容忍。观察器使用 QEMU 11.1 的实际 `include/plugins/qemu-plugin.h`，只读寄存器及 guest 内存；其额外读记录 selected native 路径、MAN native 路径，以及 DCF member mask。

`group-primary-jpg/` 是先行对照：同号 DCIM NEF+JPG 组原厂选择 JPG 为 primary，235 项原生检查通过，虚拟 MOV 进入/返回/清理通过；它不构成 RAW 锚点成功。该次旧观察器的读地址范围没有覆盖 DCF 的 `0xe...` RAM，因此缺少 mask 字段；相关两个检查为 false 是观测缺口。该目录保存当次源码、trace 和 receipt，避免以更新后的脚本冒充原始执行版本。

此前三次准备输出没有运行 guest：一次缺头文件、一次 QEMU 10.2 头与实际 11.1 回调 ABI 不匹配、一次使用了 11.1 中已改名的 include 子目录。成功使用的头为 `/path/to/user/.local/share/nikon-z8-qemu/rdw/qemu-11.1.1/include/plugins/qemu-plugin.h`。
